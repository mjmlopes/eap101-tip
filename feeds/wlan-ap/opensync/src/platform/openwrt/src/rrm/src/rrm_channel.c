/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>
#include "target.h"
#include "rrm.h"
#include <uci.h>
#include <libubox/blobmsg.h>

#include "uci.h"
#include "utils.h"

#define RRM_CHANNEL_INTERVAL     15.0

struct blob_buf b = { };
struct blob_buf del = { };
struct uci_context *uci;
int reload_config = 0;
static struct ev_timer  rrm_nf_timer;
static void rrm_nf_timer_handler(struct ev_loop *loop, ev_timer *timer, int revents);

enum {
	WIF_ATTR_PROBE_ACCEPT_RATE,
	WIF_ATTR_CLIENT_CONNECT_THRESHOLD,
	WIF_ATTR_CLIENT_DISCONNECT_THRESHOLD,
	WIF_ATTR_BEACON_RATE,
	WIF_ATTR_MCAST_RATE,
	__WIF_ATTR_MAX,
};

static const struct blobmsg_policy wifi_config_policy[__WIF_ATTR_MAX] = {
		[WIF_ATTR_PROBE_ACCEPT_RATE] = { .name = "rssi_ignore_probe_request", .type = BLOBMSG_TYPE_INT32 },
		[WIF_ATTR_CLIENT_CONNECT_THRESHOLD] = { .name = "signal_connect", .type = BLOBMSG_TYPE_INT32 },
		[WIF_ATTR_CLIENT_DISCONNECT_THRESHOLD] = { .name = "signal_stay", .type = BLOBMSG_TYPE_INT32 },
		[WIF_ATTR_BEACON_RATE] = { .name = "bcn_rate", .type = BLOBMSG_TYPE_INT32 },
		[WIF_ATTR_MCAST_RATE] = { .name = "mcast_rate", .type = BLOBMSG_TYPE_INT32 },
};

const struct uci_blob_param_list wifi_config_param = {
		.n_params = __WIF_ATTR_MAX,
		.params = wifi_config_policy,
};

#if 0
static void rrm_config_set(const char *if_name, rrm_entry_t *rrm_data)
{
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "rssi_ignore_probe_request", rrm_data->probe_resp_threshold);
	blobmsg_add_u32(&b, "signal_connect", rrm_data->client_disconnect_threshold);
	blobmsg_add_u32(&b, "signal_stay", rrm_data->client_disconnect_threshold);
	blobmsg_add_u32(&b, "bcn_rate", rrm_data->beacon_rate);
	blobmsg_add_u32(&b, "mcast_rate", rrm_data->mcast_rate);

	blob_to_uci_section(uci, "wireless", if_name, "wifi-iface",
			b.head, &wifi_config_param, NULL);

	reload_config = 1;
}
#endif

rrm_config_t* rrm_get_rrm_config(radio_type_t band)
{
	rrm_config_t *rrm_config = NULL;
	ds_tree_t *rrm_list = rrm_get_rrm_config_list();

	ds_tree_foreach(rrm_list, rrm_config)
	{
		if(rrm_config->rrm_data.freq_band == band)
		{
			return rrm_config;
		}
		else
		{
			continue;
		}
	}
	return NULL;
}

radio_entry_t* rrm_get_radio_config(radio_type_t band)
{
	rrm_radio_state_t           *radio_state = NULL;
	ds_tree_t *radio_list = rrm_get_radio_list();

	ds_tree_foreach(radio_list, radio_state)
	{
		if(radio_state->config.type == band)
		{
			return &radio_state->config;
		}
		else
		{
			continue;
		}
	}
	return NULL;
}
void get_channel_bandwidth(const char* htmode, int *channel_bandwidth)
{
	if(!strcmp(htmode, "HT20"))
		*channel_bandwidth=20;
	else if (!strcmp(htmode, "HT40"))
		*channel_bandwidth=40;
	else if(!strcmp(htmode, "HT80"))
		*channel_bandwidth=80;
}

void rrm_nf_timer_handler(struct ev_loop *loop, ev_timer *timer, int revents)
{
	(void)loop;
	(void)timer;
	(void)revents;

	rrm_radio_state_t       *radio = NULL;
	uint32_t                 noise;
	int32_t                  nf;
	int32_t                  nf_drop_threshold;
	rrm_config_t             *rrm_config;

	ds_tree_t *radio_list = rrm_get_radio_list();

	ds_tree_foreach(radio_list, radio)
	{
		noise = 0;
		rrm_config = NULL;
		nf_drop_threshold = 0;

		if (ubus_get_noise(radio->config.if_name, &noise))
			continue;

		nf = (int32_t)noise;

		if (nf > -1 || nf < -120)
			continue;

		rrm_config = rrm_get_rrm_config(radio->config.type);

		if (rrm_config == NULL)
			continue;

		if (nf < rrm_config->rrm_data.noise_lwm )
		{
			rrm_config->rrm_data.noise_lwm = nf;
			LOGD("[%s] noise_lwm set to %d", radio->config.if_name, nf);
			continue;
		}

		if (rrm_config->rrm_data.snr_percentage_drop == 0)
			continue;

		if (rrm_config->rrm_data.backup_channel == 0)
			continue;

		nf_drop_threshold = ((int32_t)(100 - rrm_config->rrm_data.snr_percentage_drop) *
				rrm_config->rrm_data.noise_lwm) / 100;

		LOGD("[%s] backup=%d nf=%d nf_lwm=%d drop=%d thresh=%d",
				radio->config.if_name,
				rrm_config->rrm_data.backup_channel,
				nf,
				rrm_config->rrm_data.noise_lwm,
				rrm_config->rrm_data.snr_percentage_drop,
				nf_drop_threshold);

		if (nf > nf_drop_threshold)
		{
			LOGI("Interference detected on [%s], switching to backup_channel=%d nf=%d nf_lwm=%d drop=%d thresh=%d",
					radio->config.if_name,
					rrm_config->rrm_data.backup_channel,
					nf,
					rrm_config->rrm_data.noise_lwm,
					rrm_config->rrm_data.snr_percentage_drop,
					nf_drop_threshold);
			int channel_bandwidth;
			int sec_chan_offset=0;
			struct mode_map *m = mode_map_get_uci(radio->schema.freq_band, get_max_channel_bw_channel(ieee80211_channel_to_frequency(rrm_config->rrm_data.backup_channel),
						radio->schema.ht_mode), radio->schema.hw_mode);
			if (m) {
				sec_chan_offset = m->sec_channel_offset;
			} else
				 LOGE("failed to get channel offset");

			get_channel_bandwidth(get_max_channel_bw_channel(ieee80211_channel_to_frequency(rrm_config->rrm_data.backup_channel),
					radio->schema.ht_mode), &channel_bandwidth);
			ubus_set_channel_switch(radio->config.if_name,
					ieee80211_channel_to_frequency(rrm_config->rrm_data.backup_channel), channel_bandwidth, sec_chan_offset);
		}
	}
}

void set_rrm_parameters(rrm_entry_t *rrm_data)
{
	ds_tree_t                       *radio_list;
	ds_tree_t                       *rrm_vif_list;
	rrm_radio_state_t               *radio = NULL;

	radio_list = rrm_get_radio_list();
	rrm_vif_list = rrm_get_vif_list();

	ds_tree_foreach(radio_list, radio)
	{
		if(radio->config.type == rrm_data->freq_band)
		{
			if (radio->schema.vif_states_len > 0) {
				int i;
				rrm_vif_state_t *vif = NULL;

				for (i = 0; i < radio->schema.vif_states_len; i++)
				{
					vif = ds_tree_find(
							rrm_vif_list,
							radio->schema.vif_states[i].uuid);

					if (vif == NULL) {
						continue;
					}

					if (strcmp(vif->schema.mode, "sta") == 0) {
						continue;
					}

					if (vif->schema.enabled == 0) {
						continue;
					}
					// rrm_config_set(vif->schema.if_name, rrm_data);
					continue;
				}
			}
		}
	}

}

static void reload_config_task(void *arg)
{
	if (reload_config) {
		uci_commit_all(uci);
		system("reload_config");
		reload_config = 0;
	}

	evsched_task_reschedule_ms(EVSCHED_SEC(5));
}

void rrm_channel_init(void)
{
	uci = uci_alloc_context();

	ev_timer_init(&rrm_nf_timer,
			rrm_nf_timer_handler,
			RRM_CHANNEL_INTERVAL,
			RRM_CHANNEL_INTERVAL);

	ev_timer_start(EV_DEFAULT, &rrm_nf_timer);
	evsched_task(&reload_config_task, NULL, EVSCHED_SEC(5));
}
