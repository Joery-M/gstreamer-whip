/*
 * Simple WHIP client
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: GPLv3
 *
 * Based on webrtc-sendrecv.c, which is released under a BSD 2-Clause
 * License and Copyright(c) 2017, Centricular:
 * https://github.com/centricular/gstwebrtc-demos/blob/master/sendrecv/gst/webrtc-sendrecv.c
 *
 */

/* Generic includes */
#include <signal.h>
#include <string.h>
#include <inttypes.h>

/* GStreamer */
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

/* HTTP/JSON stack (Janus API) */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

/* Local includes */
#include "debug.h"


/* Logging */
int whip_log_level = LOG_INFO;
gboolean whip_log_timestamps = FALSE;
gboolean whip_log_colors = TRUE;
#define WHIP_PREFIX ANSI_COLOR_CYAN"[WHIP]"ANSI_COLOR_RESET" "

/* State management */
enum whip_state {
	WHIP_STATE_DISCONNECTED = 0,
	WHIP_STATE_CONNECTING = 1,
	WHIP_STATE_CONNECTION_ERROR,
	WHIP_STATE_CONNECTED,
	WHIP_STATE_PUBLISHING,
	WHIP_STATE_OFFER_PREPARED,
	WHIP_STATE_STARTED,
	WHIP_STATE_API_ERROR,
	WHIP_STATE_ERROR
};

/* Global properties */
static GMainLoop *loop = NULL;
static GstElement *pipeline = NULL, *pc = NULL;
static const char *audio_pipe = NULL, *video_pipe = NULL;
static const char *stun_server = NULL, *turn_server = NULL;

/* API properties */
static enum whip_state state = 0;
static const char *server_url = NULL, *token = NULL;
static char *resource_url = NULL;

/* Helper methods and callbacks */
static gboolean whip_check_plugins(void);
static char *whip_json_to_string(JsonObject *object);
static gboolean whip_initialize(void);
static void whip_negotiation_needed(GstElement *element, gpointer user_data);
static void whip_offer_available(GstPromise *promise, gpointer user_data);
static void whip_candidate(GstElement *webrtc G_GNUC_UNUSED,
	guint mlineindex, char *candidate, gpointer user_data G_GNUC_UNUSED);
static void whip_connection_state(GstElement *webrtc, GParamSpec *pspec,
	gpointer user_data G_GNUC_UNUSED);
static void whip_ice_connection_state(GstElement *webrtc, GParamSpec *pspec,
	gpointer user_data G_GNUC_UNUSED);
static void whip_connect(GstWebRTCSessionDescription *offer);
static void whip_disconnect(char *reason);


/* Signal handler */
static volatile gint stop = 0;
static void whip_handle_signal(int signum) {
	WHIP_LOG(LOG_INFO, "Stopping the WHIP client...\n");
	if(g_atomic_int_compare_and_exchange(&stop, 0, 1)) {
		whip_disconnect("Shutting down");
	} else {
		g_atomic_int_inc(&stop);
		if(g_atomic_int_get(&stop) > 2)
			exit(1);
	}
}

/* Supported command-line arguments */
static GOptionEntry opt_entries[] = {
	{ "url", 'u', 0, G_OPTION_ARG_STRING, &server_url, "Address of the WHIP endpoint (required)", NULL },
	{ "token", 't', 0, G_OPTION_ARG_STRING, &token, "Authentication Bearer token to use (optional)", NULL },
	{ "audio", 'A', 0, G_OPTION_ARG_STRING, &audio_pipe, "GStreamer pipeline to use for audio (optional, required if audio-only)", NULL },
	{ "video", 'V', 0, G_OPTION_ARG_STRING, &video_pipe, "GStreamer pipeline to use for video (optional, required if video-only)", NULL },
	{ "stun-server", 'S', 0, G_OPTION_ARG_STRING, &stun_server, "STUN server to use, if any (hostname:port)", NULL },
	{ "turn-server", 'T', 0, G_OPTION_ARG_STRING, &turn_server, "TURN server to use, if any (username:password@host:port)", NULL },
	{ "log-level", 'l', 0, G_OPTION_ARG_INT, &whip_log_level, "Logging level (0=disable logging, 7=maximum log level; default: 4)", NULL },
	{ NULL },
};

/* Main application */
int main(int argc, char *argv[]) {

	/* Parse the command-line arguments */
	GError *error = NULL;
	GOptionContext *opts = g_option_context_new("-- Simple WHIP client");
	g_option_context_set_help_enabled(opts, TRUE);
	g_option_context_add_main_entries(opts, opt_entries, NULL);
	if(!g_option_context_parse(opts, &argc, &argv, &error)) {
		g_error_free(error);
		exit(1);
	}
	/* If some arguments are missing, fail */
	if(server_url == NULL || (audio_pipe == NULL && video_pipe == NULL)) {
		char *help = g_option_context_get_help(opts, TRUE, NULL);
		g_print("%s", help);
		g_free(help);
		g_option_context_free(opts);
		exit(1);
	}

	/* Logging level: default is info and no timestamps */
	if(whip_log_level == 0)
		whip_log_level = LOG_INFO;
	if(whip_log_level < LOG_NONE)
		whip_log_level = 0;
	else if(whip_log_level > LOG_MAX)
		whip_log_level = LOG_MAX;

	/* Handle SIGINT (CTRL-C), SIGTERM (from service managers) */
	signal(SIGINT, whip_handle_signal);
	signal(SIGTERM, whip_handle_signal);

	WHIP_LOG(LOG_INFO, "\n--------------------\n");
	WHIP_LOG(LOG_INFO, "Simple WHIP client\n");
	WHIP_LOG(LOG_INFO, "------------------\n\n");

	WHIP_LOG(LOG_INFO, "WHIP endpoint:  %s\n", server_url);
	WHIP_LOG(LOG_INFO, "Bearer Token:   %s\n", token ? token : "(none)");
	WHIP_LOG(LOG_INFO, "STUN server:    %s\n", stun_server ? stun_server : "(none)");
	WHIP_LOG(LOG_INFO, "TURN server:    %s\n", turn_server ? turn_server : "(none)");
	WHIP_LOG(LOG_INFO, "Audio pipeline: %s\n", audio_pipe ? audio_pipe : "(none)");
	WHIP_LOG(LOG_INFO, "Video pipeline: %s\n\n", video_pipe ? video_pipe : "(none)");

	/* Initialize gstreamer */
	gst_init(NULL, NULL);
	/* Make sure our gstreamer dependency has all we need */
	if(!whip_check_plugins())
		exit(1);

	/* Start the main Glib loop */
	loop = g_main_loop_new(NULL, FALSE);
	/* Initialize the stack (and then connect to the WHIP endpoint) */
	if(!whip_initialize())
		exit(1);

	/* Loop forever */
	g_main_loop_run(loop);
	if(loop != NULL)
		g_main_loop_unref(loop);

	/* We're done */
	if(pipeline) {
		gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);
		WHIP_LOG(LOG_INFO, WHIP_PREFIX "GStreamer pipeline stopped\n");
		gst_object_unref(pipeline);
	}

	WHIP_LOG(LOG_INFO, "\nBye!\n");
	exit(0);
}


/* Helper method to ensure GStreamer has the modules we need */
static gboolean whip_check_plugins(void) {
	/* Note: since the pipeline is dynamic, there may be more requirements... */
	const char *needed[] = {
		"opus",
		"x264",
		"vpx",
		"nice",
		"webrtc",
		"dtls",
		"srtp",
		"rtpmanager",
		"videotestsrc",
		"audiotestsrc",
		NULL
	};
	GstRegistry *registry = gst_registry_get();
	if(registry == NULL) {
		WHIP_LOG(LOG_FATAL, "No plugins registered in gstreamer\n");
		return FALSE;
	}
	gboolean ret = TRUE;

	int i = 0;
	GstPlugin *plugin = NULL;
	for(i = 0; i < g_strv_length((char **) needed); i++) {
		plugin = gst_registry_find_plugin(registry, needed[i]);
		if(plugin == NULL) {
			WHIP_LOG(LOG_FATAL, "Required gstreamer plugin '%s' not found\n", needed[i]);
			ret = FALSE;
			continue;
		}
		gst_object_unref(plugin);
	}
	return ret;
}
/* Helper method to serialize a JsonObject to a string */
static char *whip_json_to_string(JsonObject *object) {
	/* Make it the root node */
	JsonNode *root = json_node_init_object(json_node_alloc(), object);
	JsonGenerator *generator = json_generator_new();
	json_generator_set_root(generator, root);
	char *text = json_generator_to_data(generator, NULL);

	/* Release everything */
	g_object_unref(generator);
	json_node_free(root);
	return text;
}

/* Helper method to initialize the GStreamer WebRTC stack */
static gboolean whip_initialize(void) {
	/* Prepare the pipeline, using the info we got from the command line */
	char stun[255], turn[255], audio[1024], video[1024], gst_pipeline[2048];
	stun[0] = '\0';
	turn[0] = '\0';
	if(stun_server != NULL)
		g_snprintf(stun, sizeof(stun), "stun-server=stun://%s", stun_server);
	if(turn_server != NULL)
		g_snprintf(turn, sizeof(turn), "turn-server=turn://%s", turn_server);
	audio[0] = '\0';
	if(audio_pipe != NULL)
		g_snprintf(audio, sizeof(audio), "%s ! sendonly.", audio_pipe);
	video[0] = '\0';
	if(video_pipe != NULL)
		g_snprintf(video, sizeof(video), "%s ! sendonly.", video_pipe);
	g_snprintf(gst_pipeline, sizeof(gst_pipeline), "webrtcbin name=sendonly bundle-policy=%d %s %s %s %s",
		(audio_pipe && video_pipe ? 3 : 0), stun, turn, video, audio);
	/* Launch the pipeline */
	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Initializing the GStreamer pipeline:\n%s\n", gst_pipeline);
	GError *error = NULL;
	pipeline = gst_parse_launch(gst_pipeline, &error);
	if(error) {
		WHIP_LOG(LOG_ERR, "Failed to parse/launch the pipeline: %s\n", error->message);
		g_error_free(error);
		goto err;
	}

	/* Get a pointer to the PeerConnection object */
	pc = gst_bin_get_by_name(GST_BIN(pipeline), "sendonly");
	g_assert_nonnull(pc);
	/* Let's configure the function to be invoked when an SDP offer can be prepared */
	g_signal_connect(pc, "on-negotiation-needed", G_CALLBACK(whip_negotiation_needed), NULL);
	/* We need a different callback to be notified about candidates to trickle to Janus */
	g_signal_connect(pc, "on-ice-candidate", G_CALLBACK(whip_candidate), NULL);
	/* We also add a couple of callbacks to be notified about connection state changes */
	g_signal_connect(pc, "notify::connection-state", G_CALLBACK(whip_connection_state), NULL);
	g_signal_connect(pc, "notify::ice-connection-state", G_CALLBACK(whip_ice_connection_state), NULL);
	/* FIXME What about the DTLS state? */

	/* Start the pipeline */
	gst_element_set_state(pipeline, GST_STATE_READY);
	/* Lifetime is the same as the pipeline itself */
	gst_object_unref(pc);

	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Starting the GStreamer pipeline\n");
	GstStateChangeReturn ret = gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
	if(ret == GST_STATE_CHANGE_FAILURE) {
		WHIP_LOG(LOG_ERR, "Failed to set the pipeline state to playing\n");
		goto err;
	}

	/* Done */
	return TRUE;

err:
	/* If we got here, something went wrong */
	if(pipeline)
		g_clear_object(&pipeline);
	if(pc)
		pc = NULL;
	return FALSE;
}

/* Callback invoked when we need to prepare an SDP offer */
static void whip_negotiation_needed(GstElement *element, gpointer user_data) {
	if(resource_url != NULL) {
		/* We've sent an offer already, is something wrong? */
		WHIP_LOG(LOG_WARN, "GStreamer trying to create a new offer, but we don't support renegotiations yet...\n");
		return;
	}
	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Creating offer\n");
	state = WHIP_STATE_OFFER_PREPARED;
	GstPromise *promise = gst_promise_new_with_change_func(whip_offer_available, user_data, NULL);;
	g_signal_emit_by_name(pc, "create-offer", NULL, promise);
}

/* Callback invoked when we have an SDP offer ready to be sent */
static void whip_offer_available(GstPromise *promise, gpointer user_data) {
	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Offer created\n");
	/* Make sure we're in the right state */
	g_assert_cmphex(state, ==, WHIP_STATE_OFFER_PREPARED);
	g_assert_cmphex(gst_promise_wait(promise), ==, GST_PROMISE_RESULT_REPLIED);
	const GstStructure *reply = gst_promise_get_reply(promise);
	GstWebRTCSessionDescription *offer = NULL;
	gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
	gst_promise_unref(promise);

	/* Set the local description locally */
	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Setting local description\n");
	promise = gst_promise_new();
	g_signal_emit_by_name(pc, "set-local-description", offer, promise);
	gst_promise_interrupt(promise);
	gst_promise_unref(promise);

	/* Now that the offer is ready, connect to the WHIP endpoint and send it there */
	whip_connect(offer);
	gst_webrtc_session_description_free(offer);
}

/* Callback invoked when a candidate to trickle becomes available */
static void whip_candidate(GstElement *webrtc G_GNUC_UNUSED,
		guint mlineindex, char *candidate, gpointer user_data G_GNUC_UNUSED) {
	/* Make sure we're in the right state*/
	if(state < WHIP_STATE_OFFER_PREPARED) {
		whip_disconnect("Can't trickle, not in a PeerConnection");
		return;
	}

	/* Prepare the candidate JSON object */
	JsonObject *c = json_object_new();
	json_object_set_string_member(c, "candidate", candidate);
	json_object_set_int_member(c, "sdpMLineIndex", mlineindex);
	char *json_candidate = whip_json_to_string(c);
	json_object_unref(c);

	/* Send the candidate via a PATCH message */
	WHIP_LOG(LOG_VERB, WHIP_PREFIX "Sending candidate: %s\n", json_candidate);
	/* Create an HTTP connection */
	SoupSession *http_conn = soup_session_new();
	SoupMessage *msg = soup_message_new("PATCH", server_url);
	soup_message_set_request(msg, "application/trickle-ice-sdpfrag", SOUP_MEMORY_COPY,
		json_candidate, strlen(json_candidate));
	g_free(json_candidate);
	if(token != NULL) {
		/* Add an authorization header too */
		char auth[1024];
		g_snprintf(auth, sizeof(auth), "Bearer %s", token);
		soup_message_headers_append(msg->request_headers, "Authorization", auth);
	}
	/* Send the message synchronously */
	guint status = soup_session_send_message(http_conn, msg);
	if(status != 200) {
		/* Couldn't trickle? */
		WHIP_LOG(LOG_WARN, " [trickle] %u %s\n", status, msg->reason_phrase);
	}
	g_object_unref(msg);
	g_object_unref(http_conn);
}

/* Callback invoked when the connection state changes */
static void whip_connection_state(GstElement *webrtc, GParamSpec *pspec,
		gpointer user_data G_GNUC_UNUSED) {
	guint state = 0;
	g_object_get(webrtc, "connection-state", &state, NULL);
	switch(state) {
		case 1:
			WHIP_LOG(LOG_INFO, WHIP_PREFIX "PeerConnection connecting...\n");
			break;
		case 2:
			WHIP_LOG(LOG_INFO, WHIP_PREFIX "PeerConnection connected\n");
			break;
		case 4:
			WHIP_LOG(LOG_ERR, WHIP_PREFIX "PeerConnection failed\n");
			whip_disconnect("PeerConnection failed");
			break;
		case 0:
		case 3:
		case 5:
		default:
			/* We don't care (we should in case of restarts?) */
			break;
	}
}

/* Callback invoked when the ICE connection state changes */
static void whip_ice_connection_state(GstElement *webrtc, GParamSpec *pspec,
		gpointer user_data G_GNUC_UNUSED) {
	guint state = 0;
	g_object_get(webrtc, "ice-connection-state", &state, NULL);
	switch(state) {
		case 1:
			WHIP_LOG(LOG_INFO, WHIP_PREFIX "ICE connecting...\n");
			break;
		case 2:
			WHIP_LOG(LOG_INFO, WHIP_PREFIX "ICE connected\n");
			break;
		case 3:
			WHIP_LOG(LOG_INFO, WHIP_PREFIX "ICE completed\n");
			break;
		case 4:
			WHIP_LOG(LOG_ERR, WHIP_PREFIX "ICE failed\n");
			whip_disconnect("ICE failed");
			break;
		case 0:
		case 5:
		default:
			/* We don't care (we should in case of restarts?) */
			break;
	}
}

/* Helper method to connect to the WHIP endpoint */
static void whip_connect(GstWebRTCSessionDescription *offer) {
	/* Convert the SDP object to a string */
	char *sdp_offer = gst_sdp_message_as_text(offer->sdp);
	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Sending SDP offer (%zu bytes)\n", strlen(sdp_offer));
	WHIP_LOG(LOG_VERB, "%s\n", sdp_offer);

	/* Create an HTTP connection */
	SoupSession *http_conn = soup_session_new();
	SoupMessage *msg = soup_message_new("POST", server_url);
	soup_message_set_request(msg, "application/sdp", SOUP_MEMORY_COPY, sdp_offer, strlen(sdp_offer));
	if(token != NULL) {
		/* Add an authorization header too */
		char auth[1024];
		g_snprintf(auth, sizeof(auth), "Bearer %s", token);
		soup_message_headers_append(msg->request_headers, "Authorization", auth);
	}
	/* Send the message synchronously */
	guint status = soup_session_send_message(http_conn, msg);
	if(status != 201) {
		/* Didn't get the success we were expecting */
		WHIP_LOG(LOG_ERR, " [%u] %s\n", status, msg->reason_phrase);
		g_object_unref(msg);
		g_object_unref(http_conn);
		whip_disconnect("HTTP error");
		return;
	}
	/* Get the response */
	const char *content_type = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if(content_type == NULL || strcasecmp(content_type, "application/sdp")) {
		WHIP_LOG(LOG_ERR, "Unexpected content-type '%s'\n", content_type);
		g_object_unref(msg);
		g_object_unref(http_conn);
		whip_disconnect("HTTP error");
		return;
	}
	const char *answer = msg->response_body ? msg->response_body->data : NULL;
	if(answer == NULL || strstr(answer, "v=0\r\n") != answer) {
		WHIP_LOG(LOG_ERR, "Missing or invalid SDP answer\n");
		g_object_unref(msg);
		g_object_unref(http_conn);
		whip_disconnect("SDP error");
		return;
	}
	/* Parse the location header to populate the resource url */
	const char *location = soup_message_headers_get_one(msg->response_headers, "location");
	if(strstr(location, "http")) {
		/* Easy enough */
		resource_url = g_strdup(location);
	} else {
		/* Relative path */
		SoupURI *uri = soup_uri_new(server_url);
		soup_uri_set_path(uri, location);
		resource_url = soup_uri_to_string(uri, FALSE);
		soup_uri_free(uri);
	}
	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Resource URL: %s\n", resource_url);

	/* Process the SDP answer */
	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Received SDP answer (%zu bytes)\n", strlen(answer));
	WHIP_LOG(LOG_VERB, "%s\n", answer);

	/* Check if there are any candidates in the SDP: we'll need to fake trickles in case */
	if(strstr(answer, "candidate") != NULL) {
		int mlines = 0, i = 0;
		gchar **lines = g_strsplit(answer, "\r\n", -1);
		gchar *line = NULL;
		while(lines[i] != NULL) {
			line = lines[i];
			if(strstr(line, "m=") == line) {
				/* New m-line */
				mlines++;
				if(mlines > 1)	/* We only need candidates from the first one */
					break;
			} else if(mlines == 1 && strstr(line, "a=candidate") != NULL) {
				/* Found a candidate, fake a trickle */
				line += 2;
				WHIP_LOG(LOG_VERB, "  -- Found candidate: %s\n", line);
				g_signal_emit_by_name(pc, "add-ice-candidate", 0, line);
			}
			i++;
		}
		g_clear_pointer(&lines, g_strfreev);
	}
	/* Convert the SDP to something webrtcbin can digest */
	GstSDPMessage *sdp = NULL;
	int ret = gst_sdp_message_new(&sdp);
	if(ret != GST_SDP_OK) {
		/* Something went wrong */
		WHIP_LOG(LOG_ERR, "Error initializing SDP object (%d)\n", ret);
		g_object_unref(msg);
		g_object_unref(http_conn);
		whip_disconnect("SDP error");
		return;
	}
	ret = gst_sdp_message_parse_buffer((guint8 *)answer, strlen(answer), sdp);
	g_object_unref(msg);
	g_object_unref(http_conn);
	if(ret != GST_SDP_OK) {
		/* Something went wrong */
		WHIP_LOG(LOG_ERR, "Error parsing SDP buffer (%d)\n", ret);
		whip_disconnect("SDP error");
		return;
	}
	GstWebRTCSessionDescription *gst_sdp = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
	/* Set remote description on our pipeline */
	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Setting remote description\n");
	GstPromise *promise = gst_promise_new();
	g_signal_emit_by_name(pc, "set-remote-description", gst_sdp, promise);
	gst_promise_interrupt(promise);
	gst_promise_unref(promise);
}

/* Helper method to disconnect from the WHIP endpoint */
static void whip_disconnect(char *reason) {
	/* TODO*/
	WHIP_LOG(LOG_INFO, WHIP_PREFIX "Disconnecting from server (%s)\n", reason);
	if(resource_url == NULL) {
		/* FIXME Nothing to do? */
		g_main_loop_quit(loop);
		return;
	}

	/* Create an HTTP connection */
	SoupSession *http_conn = soup_session_new();
	SoupMessage *msg = soup_message_new("DELETE", resource_url);
	if(token != NULL) {
		/* Add an authorization header too */
		char auth[1024];
		g_snprintf(auth, sizeof(auth), "Bearer %s", token);
		soup_message_headers_append(msg->request_headers, "Authorization", auth);
	}
	/* Send the message synchronously */
	guint status = soup_session_send_message(http_conn, msg);
	if(status != 200) {
		WHIP_LOG(LOG_WARN, " [%u] %s\n", status, msg->reason_phrase);
	}
	g_free(resource_url);
	g_object_unref(msg);
	g_object_unref(http_conn);

	/* Done */
	g_main_loop_quit(loop);
}