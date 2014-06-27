
/*!
 * \file	sccp_pbx.c
 * \brief	SCCP PBX Asterisk Wrapper Class
 * \author	Diederik de Groot <ddegroot [at] users.sourceforge.net>
 * \note	Reworked, but based on chan_sccp code.
 * 		The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 * 		Modified by Jan Czmok and Julien Goodwin
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 * 		See the LICENSE file at the top of the source tree.
 *
 * $Date$
 * $Revision$
 */
#ifndef __PBX_IMPL_C
#define __PBX_IMPL_C

#include <config.h>
#include "common.h"
#include "sccp_pbx.h"
#include "sccp_device.h"
#include "sccp_channel.h"
#include "sccp_line.h"
#include "sccp_utils.h"
#include "sccp_features.h"
#include "sccp_conference.h"
#include "sccp_indicate.h"
#include "sccp_socket.h"

SCCP_FILE_VERSION(__FILE__, "$Revision$")

/*!
 * \brief SCCP Structure to pass data to the pbx answer thread 
 */
struct sccp_answer_conveyor_struct {
	uint32_t callid;
	sccp_linedevices_t *linedevice;
};

/*!
 * \brief Call Auto Answer Thead
 * \param data Data
 *
 * The Auto Answer thread is started by ref sccp_pbx_call if necessary
 */
static void *sccp_pbx_call_autoanswer_thread(void *data)
{
	struct sccp_answer_conveyor_struct *conveyor = data;


	int instance = 0;

	sleep(GLOB(autoanswer_ring_time));
	pthread_testcancel();

	if (!conveyor) {
		return NULL;
	}
	if (!conveyor->linedevice) {
		goto FINAL;
	}
	
	{
		AUTO_RELEASE sccp_device_t *device = sccp_device_retain(conveyor->linedevice->device);
		if (!device) {
			goto FINAL;
		}

		AUTO_RELEASE sccp_channel_t *c = sccp_channel_find_byid(conveyor->callid);
		if (!c || c->state != SCCP_CHANNELSTATE_RINGING) {
			goto FINAL;
		}

		sccp_channel_answer(device, c);

		if (GLOB(autoanswer_tone) != SKINNY_TONE_SILENCE && GLOB(autoanswer_tone) != SKINNY_TONE_NOTONE) {
			instance = sccp_device_find_index_for_line(device, c->line->name);
			sccp_dev_starttone(device, GLOB(autoanswer_tone), instance, c->callid, 0);
		}
		if (c->autoanswer_type == SCCP_AUTOANSWER_1W) {
			sccp_dev_set_microphone(device, SKINNY_STATIONMIC_OFF);
		}
	}
FINAL:
	conveyor->linedevice = conveyor->linedevice ? sccp_linedevice_release(conveyor->linedevice) : NULL;	// retained in calling thread, final release here
	sccp_free(conveyor);
	return NULL;
}

/*!
 * \brief Incoming Calls by Asterisk SCCP_Request
 * \param c SCCP Channel
 * \param dest Destination as char
 * \param timeout Timeout after which incoming call is cancelled as int
 * \return Success as int
 *
 * \todo reimplement DNDMODES, ringermode=urgent, autoanswer
 *
 * \callgraph
 * \callergraph
 * 
 * \called_from_asterisk
 * 
 * \note called with c retained
 */

int sccp_pbx_call(sccp_channel_t * c, char *dest, int timeout)
{
	if (!c) {
		return -1;
	}

	char *cid_name = NULL;
	char *cid_number = NULL;

	char suffixedNumber[255] = { '\0' };									/*!< For saving the digittimeoutchar to the logs */
	boolean_t hasSession = FALSE;

	AUTO_RELEASE sccp_line_t *l = sccp_line_retain(c->line);
	if (l) {
		sccp_linedevices_t *linedevice;

		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {

			c->subscribers++;

			if (linedevice->device->session) {
				hasSession = TRUE;
			}
		}
		SCCP_LIST_UNLOCK(&l->devices);
		if (!hasSession) {
			pbx_log(LOG_WARNING, "SCCP: weird error. The channel %d has no device connected to this line or device has no valid session\n", (c->callid));
			return -1;
		}
	} else {
		pbx_log(LOG_WARNING, "SCCP: weird error. The channel %d has no line\n", (c->callid));
		return -1;
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Asterisk request to call %s\n", l->name, PBX(getChannelName) (c));

	/* if incoming call limit is reached send BUSY */
	if ((l->incominglimit && SCCP_RWLIST_GETSIZE(&l->channels) > l->incominglimit)) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "Incoming calls limit (%d) reached on SCCP/%s... sending busy\n", l->incominglimit, l->name);
		pbx_setstate(c->owner, AST_STATE_BUSY);
		PBX(queue_control) (c->owner, AST_CONTROL_BUSY);
		return 0;
	}

	/* Reinstated this call instead of the following lines */
	if (strlen(c->callInfo.callingPartyName) > 0) {
		cid_name = strdup(c->callInfo.callingPartyName);
	}

	if (strlen(c->callInfo.callingPartyNumber) > 0) {
		cid_number = strdup(c->callInfo.callingPartyNumber);
	}
	//! \todo implement dnid, ani, ani2 and rdnis
	sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_call) asterisk callerid='%s <%s>'\n", (cid_number) ? cid_number : "", (cid_name) ? cid_name : "");

	/* Set the channel callingParty Name and Number, called Party Name and Number, original CalledParty Name and Number, Presentation */
	if (GLOB(recorddigittimeoutchar)) {
		/* The hack to add the # at the end of the incoming number
		   is only applied for numbers beginning with a 0,
		   which is appropriate for Germany and other countries using similar numbering plan.
		   The option should be generalized, moved to the dialplan, or otherwise be replaced. */
		/* Also, we require an option whether to add the timeout suffix to certain
		   enbloc dialed numbers (such as via 7970 enbloc dialing) if they match a certain pattern.
		   This would help users dial from call history lists on other phones, which do not have enbloc dialing,
		   when using shared lines. */
		if (NULL != cid_number && strlen(cid_number) > 0 && strlen(cid_number) < sizeof(suffixedNumber) - 2 && '0' == cid_number[0]) {
			strncpy(suffixedNumber, cid_number, strlen(cid_number));
			suffixedNumber[strlen(cid_number) + 0] = '#';
			suffixedNumber[strlen(cid_number) + 1] = '\0';
			sccp_channel_set_callingparty(c, cid_name, suffixedNumber);
		} else {
			sccp_channel_set_callingparty(c, cid_name, cid_number);
		}
	} else {
		sccp_channel_set_callingparty(c, cid_name, cid_number);
	}
	/* Set the channel calledParty Name and Number 7910 compatibility */
	sccp_channel_set_calledparty(c, l->cid_name, l->cid_num);
	PBX(set_connected_line) (c, c->callInfo.calledPartyNumber, c->callInfo.calledPartyName, AST_CONNECTED_LINE_UPDATE_SOURCE_UNKNOWN);

	//! \todo implement dnid, ani, ani2 and rdnis
	if (PBX(get_callerid_presence)) {
		sccp_channel_set_calleridPresenceParameter(c, PBX(get_callerid_presence) (c));
	}
	sccp_channel_display_callInfo(c);

	if (!c->ringermode) {
		c->ringermode = SKINNY_RINGTYPE_OUTSIDE;
	}
	boolean_t isRinging = FALSE;
	boolean_t hasDNDParticipant = FALSE;

	sccp_linedevices_t *linedevice;

	SCCP_LIST_LOCK(&l->devices);
	SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
		/* do we have cfwd enabled? */
		if (linedevice->cfwdAll.enabled) {
			pbx_log(LOG_NOTICE, "%s: initialize cfwd for line %s\n", linedevice->device->id, l->name);
			if (sccp_channel_forward(c, linedevice, linedevice->cfwdAll.number) == 0) {
				sccp_device_sendcallstate(linedevice->device, linedevice->lineInstance, c->callid, SKINNY_CALLSTATE_INTERCOMONEWAY, SKINNY_CALLPRIORITY_NORMAL, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
				sccp_channel_send_callinfo(linedevice->device, c);
#ifdef CS_EXPERIMENTAL
				if (sccp_strlen_zero(pbx_builtin_getvar_helper(c->owner, "FORWARDER_FOR"))) {
					struct ast_var_t *variables;
					const char *var, *val;
					char mask[25];

					ast_channel_lock(c->owner);
					sprintf(mask, "SCCP::%d", c->callid);
					AST_LIST_TRAVERSE(pbx_channel_varshead(c->owner), variables, entries) {
						if ((var = ast_var_name(variables)) && (val = ast_var_value(variables)) && (!strcmp("LINKID", var)) && (strcmp(mask, val))) {
							sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: LINKID %s\n", val);
							pbx_builtin_setvar_helper(c->owner, "__FORWARDER_FOR", val);
						}
					}
					ast_channel_unlock(c->owner);
				}
#endif
				isRinging = TRUE;
			}
			continue;
		}

		if (!linedevice->device->session) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: line device has no session\n", DEV_ID_LOG(linedevice->device));
			continue;
		}

		/* check if c->subscriptionId.number is matching deviceSubscriptionID */
		/* This means that we call only those devices on a shared line
		   which match the specified subscription id in the dial parameters. */
		if (!sccp_util_matchSubscriptionId(c, linedevice->subscriptionId.number)) {
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: device does not match subscriptionId.number c->subscriptionId.number: '%s', deviceSubscriptionID: '%s'\n", DEV_ID_LOG(linedevice->device), c->subscriptionId.number, linedevice->subscriptionId.number);
			continue;
		}

		AUTO_RELEASE sccp_channel_t *active_channel = sccp_device_getActiveChannel(linedevice->device);
		if (active_channel) {
			sccp_indicate(linedevice->device, c, SCCP_CHANNELSTATE_CALLWAITING);
			isRinging = TRUE;
		} else {

			/** check if ringermode is not urgent and device enabled dnd in reject mode */
			if (SKINNY_RINGTYPE_URGENT != c->ringermode && linedevice->device->dndFeature.enabled && linedevice->device->dndFeature.status == SCCP_DNDMODE_REJECT) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: DND active on line %s, returning Busy\n", linedevice->device->id, linedevice->line->name);
				hasDNDParticipant = TRUE;
				continue;
			}
			sccp_indicate(linedevice->device, c, SCCP_CHANNELSTATE_RINGING);
			isRinging = TRUE;
			if (c->autoanswer_type) {

				struct sccp_answer_conveyor_struct *conveyor = sccp_calloc(1, sizeof(struct sccp_answer_conveyor_struct));

				if (conveyor) {
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Running the autoanswer thread on %s\n", DEV_ID_LOG(linedevice->device), PBX(getChannelName) (c));
					conveyor->callid = c->callid;
					conveyor->linedevice = sccp_linedevice_retain(linedevice);

					sccp_threadpool_add_work(GLOB(general_threadpool), (void *) sccp_pbx_call_autoanswer_thread, (void *) conveyor);
				}
			}
		}
	}
	SCCP_LIST_UNLOCK(&l->devices);

	if (isRinging) {
//		sccp_channel_setSkinnyCallstate(c, SKINNY_CALLSTATE_RINGIN);
                sccp_channel_setChannelstate(c, SCCP_CHANNELSTATE_RINGING);
		PBX(queue_control) (c->owner, AST_CONTROL_RINGING);
	} else if (hasDNDParticipant) {
		pbx_setstate(c->owner, AST_STATE_BUSY);
		PBX(queue_control) (c->owner, AST_CONTROL_BUSY);
	} else {
		PBX(queue_control) (c->owner, AST_CONTROL_CONGESTION);
	}

	/* set linevariables */
	PBX_VARIABLE_TYPE *v = ((l) ? l->variables : NULL);

	while (c->owner && !pbx_check_hangup(c->owner) && l && v) {
		pbx_builtin_setvar_helper(c->owner, v->name, v->value);
		v = v->next;
	}

	if (cid_name) {
		free(cid_name);
	}
	if (cid_number) {
		free(cid_number);
	}

	return isRinging != TRUE;
}

/*!
 * \brief Handle Hangup Request by Asterisk
 * \param channel SCCP Channel
 * \return Success as int
 *
 * \callgraph
 * \callergraph
 * 
 * \called_from_asterisk via sccp_wrapper_asterisk.._hangup
 * 
 * \note sccp_channel should be retained in calling function
 */

int sccp_pbx_hangup(sccp_channel_t * channel)
{

	/* here the ast channel is locked */
	//sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Asterisk request to hangup channel %s\n", PBX(getChannelName)(c));
	sccp_mutex_lock(&GLOB(usecnt_lock));
	GLOB(usecnt)--;
	sccp_mutex_unlock(&GLOB(usecnt_lock));

	pbx_update_use_count();

	AUTO_RELEASE sccp_channel_t *c = sccp_channel_retain(channel);
	if (!c) {
		sccp_log_and((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: Asked to hangup channel. SCCP channel already deleted\n");
		return -1;
	}

	AUTO_RELEASE sccp_device_t *d = sccp_channel_getDevice_retained(c);
	if (d && c->state != SCCP_CHANNELSTATE_DOWN && SKINNY_DEVICE_RS_OK == d->registrationState) {
		// if (GLOB(remotehangup_tone) && d && d->state == SCCP_DEVICESTATE_OFFHOOK && c == sccp_device_getActiveChannel_nolock(d))		/* Caused active channels never to be full released */
		if (GLOB(remotehangup_tone) && d && d->state == SCCP_DEVICESTATE_OFFHOOK && c == d->active_channel) {
			sccp_dev_starttone(d, GLOB(remotehangup_tone), 0, 0, 10);
		}
		sccp_indicate(d, c, SCCP_CHANNELSTATE_ONHOOK);
	}

	AUTO_RELEASE sccp_line_t *l = sccp_line_retain(c->line);
#ifdef CS_SCCP_CONFERENCE
	if (c && c->conference) {
		c->conference = sccp_refcount_release(c->conference, __FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	if (d && d->conference) {
		d->conference = sccp_refcount_release(d->conference, __FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
#endif														// CS_SCCP_CONFERENCE

	sccp_channel_closeAllMediaTransmitAndReceive(d, c);

	// removing scheduled dialing
	c->scheduler.digittimeout = SCCP_SCHED_DEL(c->scheduler.digittimeout);

	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "%s: Current channel %s-%08x state %s(%d)\n", (d) ? DEV_ID_LOG(d) : "(null)", l ? l->name : "(null)", c->callid, sccp_channelstate2str(c->state), c->state);

	/* end callforwards */
	sccp_channel_end_forwarding_channel(c);

	/* cancel transfer if in progress */
	sccp_channel_transfer_cancel(d, c);

	/* remove call from transferee, transferer */
	sccp_linedevices_t *linedevice = NULL;

	if (&l->devices) {
		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
			AUTO_RELEASE sccp_device_t *tmpDevice = sccp_device_retain(linedevice->device);
			if (tmpDevice) {
				sccp_channel_transfer_release(tmpDevice, c);
			}
		}
		SCCP_LIST_UNLOCK(&l->devices);
	}
	/* done - remove call from transferee, transferer */

	sccp_line_removeChannel(l, c);

	if (!d) {
		/* channel is not answered, just ringin over all devices */
		/* find the first the device on which it is registered and hangup that one (__sccp_indicate_remote_device will do the rest) */
		if (&l->devices) {
			SCCP_LIST_LOCK(&l->devices);
			SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
				if (linedevice->device && SKINNY_DEVICE_RS_OK == linedevice->device->registrationState) {
					d = sccp_device_retain(linedevice->device);
					break;
				}
			}
			SCCP_LIST_UNLOCK(&l->devices);
		}
	}
	if (d) {
		d->monitorFeature.status &= ~SCCP_FEATURE_MONITOR_STATE_ACTIVE;
		sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: Reset monitor state after hangup\n", DEV_ID_LOG(d));
		sccp_feat_changed(d, NULL, SCCP_FEATURE_MONITOR);

		sccp_indicate(d, c, SCCP_CHANNELSTATE_ONHOOK);

		/* requesting statistics */
		sccp_channel_StatisticsRequest(c);
		sccp_channel_clean(c);

		sccp_feat_changed(d, NULL, SCCP_FEATURE_MONITOR);						// update monitor feature status
		return 0;
	}
	return -1;
}

/*!
 * \brief Thread to check Device Ring Back
 *
 * The Auto Answer thread is started by ref sccp_pbx_needcheckringback if necessary
 *
 * \param d SCCP Device
 */
void sccp_pbx_needcheckringback(sccp_device_t * d)
{
	if (d && d->session) {
		sccp_session_lock(d->session);
		d->session->needcheckringback = 1;
		sccp_session_unlock(d->session);
	}
}

/*!
 * \brief Answer an Asterisk Channel
 * \note we have no bridged channel at this point
 *
 * \param channel SCCCP channel
 * \return Success as int
 *
 * \callgraph
 * \callergraph
 *
 * \called_from_asterisk
 *
 * \todo masquarade does not succeed when forwarding to a dialplan extension which starts with PLAYBACK (Is this still the case, i think this might have been resolved ?? - DdG -)
 */
int sccp_pbx_answer(sccp_channel_t * channel)
{
	int res = 0;

	sccp_log((DEBUGCAT_PBX + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "SCCP: sccp_pbx_answer\n");

	/* \todo perhaps we should lock channel here. */
	AUTO_RELEASE sccp_channel_t *c = sccp_channel_retain(channel);
	if (!c) {
		return -1;
	}
	
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: sccp_pbx_answer checking parent channel\n", c->currentDeviceId);
	if (c->parentChannel) {											// containing a retained channel, final release at the end
		/* we are a forwarded call, bridge me with my parent */
		sccp_log((DEBUGCAT_PBX + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: bridge me with my parent's channel %s\n", c->currentDeviceId, PBX(getChannelName) (c));

		PBX_CHANNEL_TYPE *astForwardedChannel = c->parentChannel->owner;
		PBX_CHANNEL_TYPE *br = NULL;

		if (PBX(getChannelAppl) (c)) {
			sccp_log_and((DEBUGCAT_PBX + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_answer) %s bridging to dialplan application %s\n", c->currentDeviceId, PBX(getChannelName) (c), PBX(getChannelAppl) (c));
		}

		/* at this point we do not have a pointer to our bridge channel so we search for it -MC */
		const char *bridgePeerChannelName = pbx_builtin_getvar_helper(c->owner, CS_BRIDGEPEERNAME);
		if (!sccp_strlen_zero(bridgePeerChannelName)) {
			sccp_log_and((DEBUGCAT_PBX + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer) searching for bridgepeer by name: %s\n", bridgePeerChannelName);
			PBX(getChannelByName) (bridgePeerChannelName, &br);
		}

		/* did we find our bridge */
		pbx_log(LOG_NOTICE, "SCCP: bridge: %s\n", (br) ? pbx_channel_name(br) : " -- no bridgepeer found -- ");
		if (br) {
			/* set the channel and the bridge to state UP to fix problem with fast pickup / autoanswer */
			pbx_setstate(c->owner, AST_STATE_UP);
			pbx_setstate(br, AST_STATE_UP);

			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer) Going to Masquerade %s into %s\n", pbx_channel_name(br), pbx_channel_name(astForwardedChannel));

			if (PBX(masqueradeHelper) (br, astForwardedChannel)) {
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer) Masqueraded into %s\n", pbx_channel_name(astForwardedChannel));
				sccp_log_and((DEBUGCAT_PBX + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) bridged. channel state: ast %s\n", pbx_state2str(pbx_channel_state(c->owner)));
				sccp_log_and((DEBUGCAT_PBX + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) bridged. channel state: astForwardedChannel %s\n", pbx_state2str(pbx_channel_state(astForwardedChannel)));
				sccp_log_and((DEBUGCAT_PBX + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) bridged. channel state: br %s\n", pbx_state2str(pbx_channel_state(br)));
				sccp_log_and((DEBUGCAT_PBX + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) ============================================== \n");
#if ASTERISK_VERSION_GROUP > 106
				pbx_indicate(br, AST_CONTROL_CONNECTED_LINE);
#endif
			} else {
				pbx_log(LOG_ERROR, "(sccp_pbx_answer) Failed to masquerade bridge into forwarded channel\n");
                                
				res = -1;
			}
		} else {
			/* we have no bridge and can not make a masquerade -> end call */
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) no bridge. channel state: ast %s\n", pbx_state2str(pbx_channel_state(c->owner)));
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) no bridge. channel state: astForwardedChannel %s\n", pbx_state2str(pbx_channel_state(astForwardedChannel)));
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) ============================================== \n");

                        pbx_log(LOG_ERROR, "%s: We did not find bridge channel for call forwarding call. Hangup\n", c->currentDeviceId);
			if (pbx_channel_state(c->owner) == AST_STATE_RING && pbx_channel_state(astForwardedChannel) == AST_STATE_DOWN && PBX(getChannelPbx) (c)) {
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "SCCP: Receiver Hungup: (hasPBX: %s)\n", PBX(getChannelPbx) (c) ? "yes" : "no");
				pbx_channel_set_hangupcause(astForwardedChannel, AST_CAUSE_CALL_REJECTED);
				c->parentChannel->hangupRequest(c->parentChannel); 
			} else {
				pbx_log(LOG_ERROR, "%s: We did not find bridge channel for call forwarding call. Hangup\n", c->currentDeviceId);
				pbx_channel_set_hangupcause(astForwardedChannel, AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
				c->parentChannel->hangupRequest(c->parentChannel); 
				sccp_channel_endcall(c);
				res = -1;
			}
                        pbx_channel_set_hangupcause(astForwardedChannel, AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
                        res = -1;
		}
		// FINISH
	} else {

		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_answer) Outgoing call %s has been answered on %s@%s\n", c->currentDeviceId, PBX(getChannelName) (c), c->line->name, c->currentDeviceId);
		sccp_channel_updateChannelCapability(c);

		/*! \todo This seems like brute force, and doesn't seem to be of much use. However, I want it to be remebered
		   as I have forgotten what my actual motivation was for writing this strange code. (-DD) */
		AUTO_RELEASE sccp_device_t *d = sccp_channel_getDevice_retained(c);
		if (d) {
			sccp_indicate(d, c, SCCP_CHANNELSTATE_PROCEED);
			sccp_indicate(d, c, SCCP_CHANNELSTATE_CONNECTED);

			/** check for monitor request */
			if (d && (d->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_REQUESTED) && !(d->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_ACTIVE)) {
				pbx_log(LOG_NOTICE, "%s: request monitor\n", d->id);
				sccp_feat_monitor(d, NULL, 0, c);
			}
		}

		if (c->rtp.video.writeState & SCCP_RTP_STATUS_ACTIVE) {
			PBX(queue_control) (c->owner, AST_CONTROL_VIDUPDATE);
		}
	}
	return res;
}

/*!
 * \brief Allocate an Asterisk Channel
 * \param channel SCCP Channel
 * \param ids Void Character Pointer (either Empty / LinkedId / Channel ID, depending on the asterisk version)
 * \param parentChannel SCCP Channel for which the channel was created
 * \return 1 on Success as uint8_t
 *
 * \callgraph
 * \callergraph
 * 
 * \lock
 *  - usecnt_lock
 */
uint8_t sccp_pbx_channel_allocate(sccp_channel_t * channel, const void *ids, const PBX_CHANNEL_TYPE *parentChannel)
{
	PBX_CHANNEL_TYPE *tmp;
	AUTO_RELEASE sccp_channel_t *c = sccp_channel_retain(channel);
	if (!c) {
		return -1;
	}
	
#ifndef CS_AST_CHANNEL_HAS_CID
	char cidtmp[256];

	memset(&cidtmp, 0, sizeof(cidtmp));
#endif														// CS_AST_CHANNEL_HAS_CID

	AUTO_RELEASE sccp_line_t *l = sccp_line_retain(c->line);
	if (!l) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_channel_allocate) Unable to find line for channel %s\n", c->designator);
		pbx_log(LOG_ERROR, "SCCP: Unable to allocate asterisk channel... returning 0\n");
		return 0;
	}

	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: try to allocate channel on line: %s\n", l->name);
	/* Don't hold a sccp pvt lock while we allocate a channel */
	AUTO_RELEASE sccp_device_t *d = sccp_channel_getDevice_retained(c);
	if (d) {
		sccp_linedevices_t *linedevice;
		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
			if (linedevice->device == d) {
				break;
                        }
		}
		SCCP_LIST_UNLOCK(&l->devices);

		switch (c->calltype) {
			case SKINNY_CALLTYPE_INBOUND:
				/* append subscriptionId to cid */
				if (linedevice && !sccp_strlen_zero(linedevice->subscriptionId.number)) {
					sprintf(c->callInfo.calledPartyNumber, "%s%s", l->cid_num, linedevice->subscriptionId.number);
				} else {
					sprintf(c->callInfo.calledPartyNumber, "%s%s", l->cid_num, (l->defaultSubscriptionId.number) ? l->defaultSubscriptionId.number : "");
				}

				if (linedevice && !sccp_strlen_zero(linedevice->subscriptionId.name)) {
					sprintf(c->callInfo.calledPartyName, "%s%s", l->cid_name, linedevice->subscriptionId.name);
				} else {
					sprintf(c->callInfo.calledPartyName, "%s%s", l->cid_name, (l->defaultSubscriptionId.name) ? l->defaultSubscriptionId.name : "");
				}
				break;
			case SKINNY_CALLTYPE_FORWARD:
			case SKINNY_CALLTYPE_OUTBOUND:
				/* append subscriptionId to cid */
				if (linedevice && !sccp_strlen_zero(linedevice->subscriptionId.number)) {
					sprintf(c->callInfo.callingPartyNumber, "%s%s", l->cid_num, linedevice->subscriptionId.number);
				} else {
					sprintf(c->callInfo.callingPartyNumber, "%s%s", l->cid_num, (l->defaultSubscriptionId.number) ? l->defaultSubscriptionId.number : "");
				}

				if (linedevice && !sccp_strlen_zero(linedevice->subscriptionId.name)) {
					sprintf(c->callInfo.callingPartyName, "%s%s", l->cid_name, linedevice->subscriptionId.name);
				} else {
					sprintf(c->callInfo.callingPartyName, "%s%s", l->cid_name, (l->defaultSubscriptionId.name) ? l->defaultSubscriptionId.name : "");
				}
				break;
			case skinny_calltype_LOOKUPERROR:
				break;
		}
	} else {

		switch (c->calltype) {
			case SKINNY_CALLTYPE_INBOUND:
				sprintf(c->callInfo.calledPartyNumber, "%s%s", l->cid_num, (l->defaultSubscriptionId.number) ? l->defaultSubscriptionId.number : "");
				sprintf(c->callInfo.calledPartyName, "%s%s", l->cid_name, (l->defaultSubscriptionId.name) ? l->defaultSubscriptionId.name : "");
				break;
			case SKINNY_CALLTYPE_FORWARD:
			case SKINNY_CALLTYPE_OUTBOUND:
				sprintf(c->callInfo.callingPartyNumber, "%s%s", l->cid_num, (l->defaultSubscriptionId.number) ? l->defaultSubscriptionId.number : "");
				sprintf(c->callInfo.callingPartyName, "%s%s", l->cid_name, (l->defaultSubscriptionId.name) ? l->defaultSubscriptionId.name : "");
				break;
			case skinny_calltype_LOOKUPERROR:
				break;
		}
	}

	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:     cid_num: \"%s\"\n", c->callInfo.callingPartyNumber);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:    cid_name: \"%s\"\n", c->callInfo.callingPartyName);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: accountcode: \"%s\"\n", l->accountcode);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:       exten: \"%s\"\n", c->dialedNumber);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:     context: \"%s\"\n", l->context);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:    amaflags: \"%d\"\n", l->amaflags);
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:   chan/call: \"%s-%08x\"\n", l->name, c->callid);

	/* This should definitely fix CDR */
	//tmp = pbx_channel_alloc(1, AST_STATE_DOWN, c->callInfo.callingPartyNumber, c->callInfo.callingPartyName, l->accountcode, c->dialedNumber, l->context, l->amaflags, "SCCP/%s-%08x", l->name, c->callid);
	PBX(alloc_pbxChannel) (c, ids, parentChannel, &tmp);
	if (!tmp) {
		pbx_log(LOG_ERROR, "%s: Unable to allocate asterisk channel on line %s\n", l->id, l->name);
		return 0;
	}
	sccp_channel_updateChannelCapability(c);
	PBX(set_nativeAudioFormats) (c, c->preferences.audio, 1);

	//! \todo check locking
	/* \todo we should remove this shit. */
	char tmpName[StationMaxNameSize];

	snprintf(tmpName, sizeof(tmpName), "SCCP/%s-%08x", l->name, c->callid);
	PBX(setChannelName) (c, tmpName);

	pbx_jb_configure(tmp, &GLOB(global_jbconf));

	// \todo: Bridge?
	// \todo: Transfer?
	sccp_mutex_lock(&GLOB(usecnt_lock));
	GLOB(usecnt)++;
	sccp_mutex_unlock(&GLOB(usecnt_lock));

	pbx_update_use_count();

	if (PBX(set_callerid_number)) {
		PBX(set_callerid_number) (c, c->callInfo.callingPartyNumber);
        }
	if (PBX(set_callerid_name)) {
		PBX(set_callerid_name) (c, c->callInfo.callingPartyName);
        }
	/* asterisk needs the native formats bevore dialout, otherwise the next channel gets the whole AUDIO_MASK as requested format
	 * chan_sip dont like this do sdp processing */
	//PBX(set_nativeAudioFormats)(c, c->preferences.audio, ARRAY_LEN(c->preferences.audio));

	// export sccp informations in asterisk dialplan
	if (d) {
		pbx_builtin_setvar_helper(tmp, "SCCP_DEVICE_MAC", d->id);
		pbx_builtin_setvar_helper(tmp, "SCCP_DEVICE_IP", d->session ? sccp_socket_stringify_addr(&d->session->sin) : "");
		pbx_builtin_setvar_helper(tmp, "SCCP_DEVICE_TYPE", skinny_devicetype2str(d->skinny_type));
	}
	sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "%s: Allocated asterisk channel %s-%d\n", (l) ? l->id : "(null)", (l) ? l->name : "(null)", c->callid);
	return 1;
}

/*!
 * \brief Schedule Asterisk Dial
 * \param data Data as constant
 * \return Success as int
 * 
 * \called_from_asterisk
 */
int sccp_pbx_sched_dial(const void *data)
{
	AUTO_RELEASE sccp_channel_t *c = sccp_channel_retain((sccp_channel_t *) data);
	if (c && c->owner && !PBX(getChannelPbx) (c)) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: Timeout for call '%d'. Going to dial '%s'\n", c->callid, c->dialedNumber);
		c->scheduler.digittimeout = 0;
		sccp_pbx_softswitch(c);
	}
	return 0;
}

/*!
 * \brief Asterisk Helper
 * \param c SCCP Channel as sccp_channel_t
 * \return Success as int
 */
sccp_extension_status_t sccp_pbx_helper(sccp_channel_t * c)
{
	sccp_extension_status_t extensionStatus;

	if (!sccp_strlen_zero(c->dialedNumber)) {
		if (GLOB(recorddigittimeoutchar) && GLOB(digittimeoutchar) == c->dialedNumber[strlen(c->dialedNumber) - 1]) {
			/* we finished dialing with digit timeout char */
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: We finished dialing with digit timeout char %s\n", c->dialedNumber);
			return SCCP_EXTENSION_EXACTMATCH;
		}
	}

	if ((c->ss_action != SCCP_SS_GETCBARGEROOM) && (c->ss_action != SCCP_SS_GETMEETMEROOM)
#ifdef CS_SCCP_CONFERENCE
		&& (c->ss_action != SCCP_SS_GETCONFERENCEROOM)
#endif
	) {

		//! \todo check overlap feature status -MC
		extensionStatus = PBX(extension_status) (c);
		AUTO_RELEASE sccp_device_t *d = sccp_channel_getDevice_retained(c);
		if (d) {
			if (((d->overlapFeature.enabled && !extensionStatus) || (!d->overlapFeature.enabled && !extensionStatus))) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: %s Matches More\n", c->dialedNumber);
				return SCCP_EXTENSION_MATCHMORE;
			}
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: %s Match %s\n", c->dialedNumber, extensionStatus == SCCP_EXTENSION_EXACTMATCH ? "Exactly" : "More");
		}
		return extensionStatus;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: %s Does Exists\n", c->dialedNumber);
	return SCCP_EXTENSION_NOTEXISTS;
}

/*!
 * \brief Handle Soft Switch
 * \param channel SCCP Channel as sccp_channel_t
 * \todo clarify Soft Switch Function
 */
void *sccp_pbx_softswitch(sccp_channel_t * channel)
{
	PBX_CHANNEL_TYPE *pbx_channel = NULL;
	PBX_VARIABLE_TYPE *v = NULL;

	{
		AUTO_RELEASE sccp_channel_t *c = sccp_channel_retain(channel);
		if (!c) {
			pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <channel> available. Returning from dial thread.\n");
			goto EXIT_FUNC;
		}

		/* Reset Enbloc Dial Emulation */
		c->enbloc.deactivate = 0;
		c->enbloc.totaldigittime = 0;
		c->enbloc.totaldigittimesquared = 0;
		c->enbloc.digittimeout = GLOB(digittimeout) * 1000;

		/* prevent softswitch from being executed twice (Pavel Troller / 15-Oct-2010) */
		if (PBX(getChannelPbx) (c)) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_softswitch) PBX structure already exists. Dialing instead of starting.\n");
			/* If there are any digits, send them instead of starting the PBX */
			if (!sccp_strlen_zero(c->dialedNumber)) {
				sccp_pbx_senddigits(c, c->dialedNumber);
				sccp_channel_set_calledparty(c, "", c->dialedNumber);
			}
			goto EXIT_FUNC;
		}

		if (!c->owner) {
			pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No PBX <channel> available. Returning from dial thread.\n");
			goto EXIT_FUNC;
		}
		pbx_channel = pbx_channel_ref(c->owner);

		/* removing scheduled dialing */
		c->scheduler.digittimeout = SCCP_SCHED_DEL(c->scheduler.digittimeout);

		/* we should just process outbound calls, let's check calltype */
		if (c->calltype != SKINNY_CALLTYPE_OUTBOUND) {
			pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) This function is for outbound calls only. Exiting\n");
			goto EXIT_FUNC;
		}

		/* assume d is the channel's device */
		/* does it exists ? */
		AUTO_RELEASE sccp_device_t *d = sccp_channel_getDevice_retained(c);
		if (!d) {
			pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <device> available. Returning from dial thread. Exiting\n");
			goto EXIT_FUNC;
		}

		/* we don't need to check for a device type but just if the device has an id, otherwise back home  -FS */
		if (!d->id || sccp_strlen_zero(d->id)) {
			pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <device> identifier available. Returning from dial thread. Exiting\n");
			goto EXIT_FUNC;
		}

		AUTO_RELEASE sccp_line_t *l = sccp_line_retain(c->line);
		if (!l) {
			pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <line> available. Returning from dial thread. Exiting\n");
			c->hangupRequest(c);
			goto EXIT_FUNC;
		}
		uint8_t instance = sccp_device_find_index_for_line(d, l->name);

		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) New call on line %s\n", DEV_ID_LOG(d), l->name);

		/* assign callerid name and number */
		//sccp_channel_set_callingparty(c, l->cid_name, l->cid_num);

		// we use shortenedNumber but why ???
		// If the timeout digit has been used to terminate the number
		// and this digit shall be included in the phone call history etc (recorddigittimeoutchar is true)
		// we still need to dial the number without the timeout char in the pbx
		// so that we don't dial strange extensions with a trailing characters.
		char shortenedNumber[256] = { '\0' };
		sccp_copy_string(shortenedNumber, c->dialedNumber, sizeof(shortenedNumber));
		unsigned int len = strlen(shortenedNumber);

		assert(strlen(c->dialedNumber) == len);

		if (len > 0 && GLOB(digittimeoutchar) == shortenedNumber[len - 1]) {
			shortenedNumber[len - 1] = '\0';

			// If we don't record the timeoutchar in the logs, we remove it from the sccp channel structure
			// Later, the channel dialed number is used for directories, etc.,
			// and the shortened number is used for dialing the actual call via asterisk pbx.
			if (!GLOB(recorddigittimeoutchar)) {
				c->dialedNumber[len - 1] = '\0';
			}
		}

		/* This will choose what to do */
		switch (c->ss_action) {
			case SCCP_SS_GETFORWARDEXTEN:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Forward Extension\n", d->id);
				if (!sccp_strlen_zero(shortenedNumber)) {
					sccp_line_cfwd(l, d, c->ss_data, shortenedNumber);
				}
				sccp_channel_endcall(c);
				goto EXIT_FUNC;										// leave simple switch without dial
#ifdef CS_SCCP_PICKUP
			case SCCP_SS_GETPICKUPEXTEN:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Pickup Extension\n", d->id);
				// like we're dialing but we're not :)
				sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
				sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
				sccp_channel_send_callinfo(d, c);
				sccp_dev_clearprompt(d, instance, c->callid);
				sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);

				if (!sccp_strlen_zero(shortenedNumber)) {
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_softswitch) Asterisk request to pickup exten '%s'\n", shortenedNumber);
					if (!sccp_feat_directed_pickup(c, shortenedNumber)) {
	// 					sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
					}
					PBX(set_callstate) (c, AST_STATE_UP);
				} else {
					// without a number we can also close the call. Isn't it true ?
					sccp_channel_endcall(c);
				}
				goto EXIT_FUNC;										// leave simpleswitch without dial
#endif															// CS_SCCP_PICKUP
#ifdef CS_SCCP_CONFERENCE
			case SCCP_SS_GETCONFERENCEROOM:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Conference request\n", d->id);
				if (c->owner && !pbx_check_hangup(c->owner)) {
					sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
					sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
					sccp_channel_setChannelstate(c, SCCP_CHANNELSTATE_PROCEED);
					PBX(set_callstate) (channel, AST_STATE_UP);
					if (!d->conference) {
						d->conference = sccp_conference_create(d, c);
						sccp_indicate(d, c, SCCP_CHANNELSTATE_CONNECTEDCONFERENCE);
					} else {
						pbx_log(LOG_NOTICE, "%s: There is already a conference running on this device.\n", DEV_ID_LOG(d));
						sccp_channel_endcall(c);
						goto EXIT_FUNC;
					}
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Start Conference\n", d->id);
					sccp_feat_conference_start(d, l, instance, c);
				}
				goto EXIT_FUNC;
#endif															// CS_SCCP_CONFERENCE
			case SCCP_SS_GETMEETMEROOM:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Meetme request\n", d->id);
				if (!sccp_strlen_zero(shortenedNumber) && !sccp_strlen_zero(c->line->meetmenum)) {
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Meetme request for room '%s' on extension '%s'\n", d->id, shortenedNumber, c->line->meetmenum);
					if (c->owner && !pbx_check_hangup(c->owner)) {
						pbx_builtin_setvar_helper(c->owner, "SCCP_MEETME_ROOM", shortenedNumber);
					}
					sccp_copy_string(shortenedNumber, c->line->meetmenum, sizeof(shortenedNumber));

					//sccp_copy_string(c->dialedNumber, SKINNY_DISP_CONFERENCE, sizeof(c->dialedNumber));
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Start Meetme Thread\n", d->id);
					sccp_feat_meetme_start(c);							/* Copied from Federico Santulli */
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Meetme Thread Started\n", d->id);
					goto EXIT_FUNC;
				} else {
					// without a number we can also close the call. Isn't it true ?
					sccp_channel_endcall(c);
					goto EXIT_FUNC;
				}
				break;
			case SCCP_SS_GETBARGEEXTEN:
				// like we're dialing but we're not :)
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Barge Extension\n", d->id);
				sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
				sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
				sccp_channel_send_callinfo(d, c);

				sccp_dev_clearprompt(d, instance, c->callid);
				sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);
				if (!sccp_strlen_zero(shortenedNumber)) {
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Device request to barge exten '%s'\n", d->id, shortenedNumber);
					if (sccp_feat_barge(c, shortenedNumber)) {
						sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
					}
				} else {
					// without a number we can also close the call. Isn't it true ?
					sccp_channel_endcall(c);
				}
				goto EXIT_FUNC;										// leave simpleswitch without dial
			case SCCP_SS_GETCBARGEROOM:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Conference Barge Extension\n", d->id);
				// like we're dialing but we're not :)
				sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
				sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
				sccp_channel_send_callinfo(d, c);
				sccp_dev_clearprompt(d, instance, c->callid);
				sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);
				if (!sccp_strlen_zero(shortenedNumber)) {
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Device request to barge conference '%s'\n", d->id, shortenedNumber);
					if (sccp_feat_cbarge(c, shortenedNumber)) {
						sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
					}
				} else {
					// without a number we can also close the call. Isn't it true ?
					sccp_channel_endcall(c);
				}
				goto EXIT_FUNC;										// leave simpleswitch without dial
			case SCCP_SS_DIAL:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Dial Extension %s\n", d->id, shortenedNumber);

				sccp_copy_string(c->callInfo.calledPartyNumber, shortenedNumber, sizeof(c->callInfo.calledPartyNumber));
				sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
				break;
		}

		/* set private variable */
		if (pbx_channel && !pbx_check_hangup(pbx_channel)) {
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_softswitch) set variable SKINNY_PRIVATE to: %s\n", c->privacy ? "1" : "0");
			if (c->privacy) {

				//pbx_channel->cid.cid_pres = AST_PRES_PROHIB_USER_NUMBER_NOT_SCREENED;
				sccp_channel_set_calleridPresenceParameter(c, CALLERID_PRESENCE_FORBIDDEN);
			}

			uint32_t result = d->privacyFeature.status & SCCP_PRIVACYFEATURE_CALLPRESENT;

			result |= c->privacy;
			if (d->privacyFeature.enabled && result) {
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_softswitch) set variable SKINNY_PRIVATE to: %s\n", "1");
				pbx_builtin_setvar_helper(pbx_channel, "SKINNY_PRIVATE", "1");
			} else {
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_softswitch) set variable SKINNY_PRIVATE to: %s\n", "0");
				pbx_builtin_setvar_helper(pbx_channel, "SKINNY_PRIVATE", "0");
			}
		}

		/* set devicevariables */
		v = ((d) ? d->variables : NULL);
		while (pbx_channel && !pbx_check_hangup(pbx_channel) && d && v) {
			pbx_builtin_setvar_helper(pbx_channel, v->name, v->value);
			v = v->next;
		}

		/* set linevariables */
		v = ((l) ? l->variables : NULL);
		while (pbx_channel && !pbx_check_hangup(pbx_channel) && l && v) {
			pbx_builtin_setvar_helper(pbx_channel, v->name, v->value);
			v = v->next;
		}

		PBX(setChannelExten) (c, shortenedNumber);

		sccp_channel_set_calledparty(c, "", shortenedNumber);

#if 0		/* Remarked out by Pavel Troller for the earlyrtp immediate implementation. Checking if there will be negative fall out. 
		It might have to check the device->earlyrtp state, to do the correct thing (Let's see). */

		/* proceed call state is needed to display the called number. The phone will not display callinfo in offhook state */
		sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
		sccp_channel_send_callinfo(d, c);

		sccp_dev_clearprompt(d, instance, c->callid);
		sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);
#endif

		/*! \todo DdG: Extra wait time is incurred when checking pbx_exists_extension, when a wrong number is dialed. storing extension_exists status for sccp_log use */
		int extension_exists = SCCP_EXTENSION_NOTEXISTS;

		if (!sccp_strlen_zero(shortenedNumber) && (pbx_channel && !pbx_check_hangup(pbx_channel)) &&
		/*  && (extension_exists = pbx_exists_extension(pbx_channel, pbx_channel_context(pbx_channel), shortenedNumber, 1, l->cid_num)) */
		    ((extension_exists = PBX(extension_status(c)) != SCCP_EXTENSION_NOTEXISTS))
		    ) {
			/* found an extension, let's dial it */
			sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x is dialing number %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
			
			sccp_copy_string(c->callInfo.calledPartyNumber, shortenedNumber, sizeof(c->callInfo.calledPartyNumber));
			/* Answer dialplan command works only when in RINGING OR RING ast_state */
			PBX(set_callstate) (c, AST_STATE_RING);

			enum ast_pbx_result pbxStartResult = pbx_pbx_start(pbx_channel);

			/* \todo replace AST_PBX enum using pbx_impl wrapper enum */
			switch (pbxStartResult) {
				case AST_PBX_FAILED:
					pbx_log(LOG_ERROR, "%s: (sccp_pbx_softswitch) channel %s-%08x failed to start new thread to dial %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
					/* \todo change indicate to something more suitable */
					sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
					break;
				case AST_PBX_CALL_LIMIT:
					pbx_log(LOG_WARNING, "%s: (sccp_pbx_softswitch) call limit reached for channel %s-%08x failed to start new thread to dial %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
					sccp_indicate(d, c, SCCP_CHANNELSTATE_CONGESTION);
					break;
				case AST_PBX_SUCCESS:
					sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) pbx started\n", DEV_ID_LOG(d));
#ifdef CS_MANAGER_EVENTS
					if (GLOB(callevents)) {
						manager_event(EVENT_FLAG_SYSTEM, "ChannelUpdate", "Channel: %s\r\nUniqueid: %s\r\nChanneltype: %s\r\nSCCPdevice: %s\r\nSCCPline: %s\r\nSCCPcallid: %08x\r\nSCCPCallDesignator: %s\r\n", (pbx_channel) ? pbx_channel_name(pbx_channel) : "(null)", (pbx_channel) ? pbx_channel_uniqueid(pbx_channel) : "(null)", "SCCP", (d) ? DEV_ID_LOG(d) : "(null)", (l && l->name) ? l->name : "(null)", (c && c->callid) ? c->callid : 0, c  ? c->designator : "(null)");
					}
#endif															// CS_MANAGER_EVENTS
					break;
			}
			sccp_device_setLastNumberDialed(d, shortenedNumber);
		} else {
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x shortenedNumber: %s, pbx_check_hangup(chan): %d, extension exists: %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber, (pbx_channel && pbx_check_hangup(pbx_channel)), (extension_exists != SCCP_EXTENSION_NOTEXISTS) ? "TRUE" : "FALSE");
			pbx_log(LOG_NOTICE, "%s: Call from '%s' to extension '%s', rejected because the extension could not be found in context '%s'\n", DEV_ID_LOG(d), l->name, shortenedNumber, pbx_channel ? pbx_channel_context(pbx_channel) : "pbx_channel==NULL");
			/* timeout and no extension match */
			sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
		}

		sccp_log((DEBUGCAT_PBX + DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) quit\n", DEV_ID_LOG(d));
	}
EXIT_FUNC:
	if (pbx_channel) {
		pbx_channel_unref(pbx_channel);
	}
	return NULL;
}

/*!
 * \brief Send Digit to Asterisk
 * \param c SCCP Channel
 * \param digit Digit as char
 */
void sccp_pbx_senddigit(sccp_channel_t * c, char digit)
{
        if (PBX(send_digit)) {
                PBX(send_digit) (c, digit);
        }
}

/*!
 * \brief Send Multiple Digits to Asterisk
 * \param c SCCP Channel
 * \param digits Multiple Digits as char
 */
void sccp_pbx_senddigits(sccp_channel_t * c, const char *digits)
{
        if (PBX(send_digits)) {
		PBX(send_digits) (c, digits);
        }
}

#if 0
/*!
 * \brief Handle Dialplan Transfer
 *
 * This will allow asterisk to transfer an SCCP Channel via the dialplan transfer function
 *
 * \param ast Asterisk Channel
 * \param dest Destination as char *
 * \return result as int
 *
 * \test Dialplan Transfer Needs to be tested
 * \todo pbx_transfer needs to be implemented correctly
 * 
 * \called_from_asterisk
 */
int sccp_pbx_transfer(PBX_CHANNEL_TYPE * ast, const char *dest)
{
	int res = 0;


	if (dest == NULL) {											/* functions below do not take a NULL */
		dest = "";
		return -1;
	}

	AUTO_RELEASE sccp_channel_t *c = get_sccp_channel_from_pbx_channel(ast);
	if (!c) {
		return -1;
	}

	/*
	   sccp_device_t *d;
	   sccp_channel_t *newcall;
	 */

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "Transferring '%s' to '%s'\n", PBX(getChannelName) (c), dest);
	if (pbx_channel_state(ast) == AST_STATE_RING) {
		//! \todo Blindtransfer needs to be implemented correctly

		/*
		   res = sccp_blindxfer(p, dest);
		 */
		res = -1;
	} else {
		//! \todo Transfer needs to be implemented correctly

		/*
		   res=sccp_channel_transfer(p,dest);
		 */
		res = -1;
	}
	return res;
}
#endif

#endif
