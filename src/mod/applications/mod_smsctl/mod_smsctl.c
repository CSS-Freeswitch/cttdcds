/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 * Raymond Chandler <intralanman@freeswitch.org>
 *
 * mod_smsctl.c -- Abstract SMSCTL
 *
 */
#include <switch.h>
/* change@suy:2021-1-3 */
#define SMS_CHAT_PROTO "GLOBAL_SMSCTL"
#define MY_EVENT_SEND_MESSAGE "SMSCTL::SEND_MESSAGE"
#define MY_EVENT_DELIVERY_REPORT "SMSCTL::DELIVERY_REPORT"

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_smsctl_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_smsctl_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_smsctl_load);
SWITCH_MODULE_DEFINITION(mod_smsctl, mod_smsctl_load, mod_smsctl_shutdown, NULL);
/* change end */


static void send_report(switch_event_t *event, const char * Status) {
	switch_event_t *report = NULL;
	switch_event_header_t *header;

	if (switch_event_create_subclass(&report, SWITCH_EVENT_CUSTOM, MY_EVENT_DELIVERY_REPORT) == SWITCH_STATUS_SUCCESS) {

		switch_event_add_header_string(report, SWITCH_STACK_BOTTOM, "Status", Status);


		for (header = event->headers; header; header = header->next) {
			if (!strcmp(header->name, "Event-Subclass")) {
				continue;
			}
			if (!strcmp(header->name, "Event-Name")) {
				continue;
			}
	        if (header->idx) {
	            int i;
	            for (i = 0; i < header->idx; i++) {
	                switch_event_add_header_string(report, SWITCH_STACK_PUSH, header->name, header->array[i]);
	            }
	        } else {
	            switch_event_add_header_string(report, SWITCH_STACK_BOTTOM, header->name, header->value);
	        }
		}
		switch_event_fire(&report);
	}
}

static void event_handler(switch_event_t *event)
{
	const char *dest_proto = switch_event_get_header(event, "dest_proto");
	const char *check_failure = switch_event_get_header(event, "Delivery-Failure");
	const char *check_nonblocking = switch_event_get_header(event, "Nonblocking-Delivery");

	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "skip_global_process", "true");

	if (switch_true(check_failure)) {

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Delivery Failure\n");
		DUMP_EVENT(event);
		send_report(event, "Failure");
		return;
	} else if ( check_failure && switch_false(check_failure) ) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "SMS Delivery Success\n");
		send_report(event, "Success");
		return;
	} else if ( check_nonblocking && switch_true(check_nonblocking) ) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "SMS Delivery assumed successful due to being sent in non-blocking manner\n");
		send_report(event, "Accepted");
		return;
	}

	switch_core_chat_send(dest_proto, event);
}

typedef enum {
	BREAK_ON_TRUE,
	BREAK_ON_FALSE,
	BREAK_ALWAYS,
	BREAK_NEVER
} break_t;


#define check_tz()														\
	do {																\
		tzoff = switch_event_get_header(event, "tod_tz_offset");		\
		tzname_ = switch_event_get_header(event, "timezone");			\
		if (!zstr(tzoff) && switch_is_number(tzoff)) {					\
			offset = atoi(tzoff);										\
			break;														\
		} else {														\
			tzoff = NULL;												\
		}																\
	} while(tzoff)

static int parse_exten(switch_event_t *event, switch_xml_t xexten, switch_event_t **extension)
{
	switch_xml_t xcond, xaction, xexpression;
	char *exten_name = (char *) switch_xml_attr(xexten, "name");
	int proceed = 0;
	char *expression_expanded = NULL, *field_expanded = NULL;
	switch_regex_t *re = NULL;
	const char *to = switch_event_get_header(event, "to");
	const char *tzoff = NULL, *tzname_ = NULL;
	int offset = 0;

	check_tz();

	if (!to) {
		to = "nobody";
	}

	if (!exten_name) {
		exten_name = "_anon_";
	}

	for (xcond = switch_xml_child(xexten, "condition"); xcond; xcond = xcond->next) {
		char *field = NULL;
		char *do_break_a = NULL;
		char *expression = NULL;
		const char *field_data = NULL;
		int ovector[30];
		switch_bool_t anti_action = SWITCH_TRUE;
		break_t do_break_i = BREAK_ON_FALSE;
		int time_match;

		check_tz();
		time_match = switch_xml_std_datetime_check(xcond, tzoff ? &offset : NULL, tzname_);

		switch_safe_free(field_expanded);
		switch_safe_free(expression_expanded);

		if (switch_xml_child(xcond, "condition")) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Nested conditions are not allowed!\n");
			proceed = 1;
			goto done;
		}

		field = (char *) switch_xml_attr(xcond, "field");

		if ((xexpression = switch_xml_child(xcond, "expression"))) {
			expression = switch_str_nil(xexpression->txt);
		} else {
			expression = (char *) switch_xml_attr_soft(xcond, "expression");
		}

		if ((expression_expanded = switch_event_expand_headers(event, expression)) == expression) {
			expression_expanded = NULL;
		} else {
			expression = expression_expanded;
		}

		if ((do_break_a = (char *) switch_xml_attr(xcond, "break"))) {
			if (!strcasecmp(do_break_a, "on-true")) {
				do_break_i = BREAK_ON_TRUE;
			} else if (!strcasecmp(do_break_a, "on-false")) {
				do_break_i = BREAK_ON_FALSE;
			} else if (!strcasecmp(do_break_a, "always")) {
				do_break_i = BREAK_ALWAYS;
			} else if (!strcasecmp(do_break_a, "never")) {
				do_break_i = BREAK_NEVER;
			} else {
				do_break_a = NULL;
			}
		}

		if (time_match == 1) {
			switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN, SWITCH_LOG_DEBUG,
							  "Chatplan: %s Date/Time Match (PASS) [%s] break=%s\n",
							  to, exten_name, do_break_a ? do_break_a : "on-false");
			anti_action = SWITCH_FALSE;
		} else if (time_match == 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN, SWITCH_LOG_DEBUG,
							  "Chatplan: %s Date/Time Match (FAIL) [%s] break=%s\n",
							  to, exten_name, do_break_a ? do_break_a : "on-false");
		}

		if (field) {
			if (strchr(field, '$')) {
				if ((field_expanded = switch_event_expand_headers(event, field)) == field) {
					field_expanded = NULL;
					field_data = field;
				} else {
					field_data = field_expanded;
				}
			} else {
				field_data = switch_event_get_header(event, field);
			}
			if (!field_data) {
				field_data = "";
			}

			if ((proceed = switch_regex_perform(field_data, expression, &re, ovector, sizeof(ovector) / sizeof(ovector[0])))) {
				switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN, SWITCH_LOG_DEBUG,
								  "Chatplan: %s Regex (PASS) [%s] %s(%s) =~ /%s/ break=%s\n",
								  to, exten_name, field, field_data, expression, do_break_a ? do_break_a : "on-false");
				anti_action = SWITCH_FALSE;
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN, SWITCH_LOG_DEBUG,
								  "Chatplan: %s Regex (FAIL) [%s] %s(%s) =~ /%s/ break=%s\n",
								  to, exten_name, field, field_data, expression, do_break_a ? do_break_a : "on-false");
			}
		} else if (time_match == -1) {
			switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN, SWITCH_LOG_DEBUG,
							  "Chatplan: %s Absolute Condition [%s]\n", to, exten_name);
			anti_action = SWITCH_FALSE;
		}

		if (anti_action) {
			for (xaction = switch_xml_child(xcond, "anti-action"); xaction; xaction = xaction->next) {
				const char *application = switch_xml_attr_soft(xaction, "application");
				const char *loop = switch_xml_attr(xaction, "loop");
				const char *data;
				const char *inline_ = switch_xml_attr_soft(xaction, "inline");
				int xinline = switch_true(inline_);
				int loop_count = 1;

				if (!zstr(xaction->txt)) {
					data = xaction->txt;
				} else {
					data = (char *) switch_xml_attr_soft(xaction, "data");
				}

				if (!*extension) {
					if ((switch_event_create(extension, SWITCH_EVENT_CLONE)) != SWITCH_STATUS_SUCCESS) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error!\n");
						abort();
					}
				}

				if (loop) {
					loop_count = atoi(loop);
				}

				for (;loop_count > 0; loop_count--) {
					switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN, SWITCH_LOG_DEBUG,
							"Chatplan: %s ANTI-Action %s(%s) %s\n", to, application, data, xinline ? "INLINE" : "");

					if (xinline) {
						switch_core_execute_chat_app(event, application, data);
					} else {
						switch_event_add_header_string(*extension, SWITCH_STACK_BOTTOM, application, zstr(data) ? "__undef" : data);
					}
				}
				proceed = 1;
			}
		} else {
			if (field && strchr(expression, '(')) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "DP_MATCH", NULL);
				switch_capture_regex(re, proceed, field_data, ovector, "DP_MATCH", switch_regex_set_event_header_callback, event);
			}

			for (xaction = switch_xml_child(xcond, "action"); xaction; xaction = xaction->next) {
				char *application = (char *) switch_xml_attr_soft(xaction, "application");
				const char *loop = switch_xml_attr(xaction, "loop");
				char *data = NULL;
				char *substituted = NULL;
				uint32_t len = 0;
				char *app_data = NULL;
				const char *inline_ = switch_xml_attr_soft(xaction, "inline");
				int xinline = switch_true(inline_);
				int loop_count = 1;

				if (!zstr(xaction->txt)) {
					data = xaction->txt;
				} else {
					data = (char *) switch_xml_attr_soft(xaction, "data");
				}

				if (field && strchr(expression, '(')) {
					len = (uint32_t) (strlen(data) + strlen(field_data) + 10) * proceed;
					if (!(substituted = (char *) malloc(len))) {
						abort();
					}
					memset(substituted, 0, len);
					switch_perform_substitution(re, proceed, data, field_data, substituted, len, ovector);
					app_data = substituted;
				} else {
					app_data = data;
				}

				if (!*extension) {
					if ((switch_event_create(extension, SWITCH_EVENT_CLONE)) != SWITCH_STATUS_SUCCESS) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error!\n");
						abort();
					}
				}

				if (loop) {
					loop_count = atoi(loop);
				}
				for (;loop_count > 0; loop_count--) {
					switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN, SWITCH_LOG_DEBUG,
							"Chatplan: %s Action %s(%s) %s\n", to, application, app_data, xinline ? "INLINE" : "");

					if (xinline) {
						switch_core_execute_chat_app(event, application, app_data);
					} else {
						switch_event_add_header_string(*extension, SWITCH_STACK_BOTTOM, application, zstr(app_data) ? "__undef" : app_data);
					}
				}
				switch_safe_free(substituted);
			}
		}
		switch_regex_safe_free(re);

		if (((anti_action == SWITCH_FALSE && do_break_i == BREAK_ON_TRUE) ||
			 (anti_action == SWITCH_TRUE && do_break_i == BREAK_ON_FALSE)) || do_break_i == BREAK_ALWAYS) {
			break;
		}
	}

  done:
	switch_regex_safe_free(re);
	switch_safe_free(field_expanded);
	switch_safe_free(expression_expanded);
	return proceed;
}


static switch_event_t *chatplan_hunt(switch_event_t *event)
{
	switch_event_t *extension = NULL;
	switch_xml_t alt_root = NULL, cfg, xml = NULL, xcontext, xexten = NULL;
	const char *alt_path;
	const char *context;
	const char *from;
	const char *to;

	if (!(context = switch_event_get_header(event, "context"))) {
		context = "default";
	}

	if (!(from = switch_event_get_header(event, "from_user"))) {
		from = switch_event_get_header(event, "from");
	}

	if (!(to = switch_event_get_header(event, "to_user"))) {
		to = switch_event_get_header(event, "to");
	}

	alt_path = switch_event_get_header(event, "alt_path");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Processing text message %s->%s in context %s\n", from, to, context);

	/* get our handle to the "chatplan" section of the config */

	if (!zstr(alt_path)) {
		switch_xml_t conf = NULL, tag = NULL;
		if (!(alt_root = switch_xml_parse_file_simple(alt_path))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of [%s] failed\n", alt_path);
			goto done;
		}

		if ((conf = switch_xml_find_child(alt_root, "section", "name", "chatplan")) && (tag = switch_xml_find_child(conf, "chatplan", NULL, NULL))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Getting chatplan from alternate path: %s\n", alt_path);
			xml = alt_root;
			cfg = tag;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of chatplan failed\n");
			goto done;
		}
	} else {
		if (switch_xml_locate("chatplan", NULL, NULL, NULL, &xml, &cfg, event, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of chatplan failed\n");
			goto done;
		}
	}

	/* get a handle to the context tag */
	if (!(xcontext = switch_xml_find_child(cfg, "context", "name", context))) {
		if (!(xcontext = switch_xml_find_child(cfg, "context", "name", "global"))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Context %s not found\n", context);
			goto done;
		}
	}

	xexten = switch_xml_child(xcontext, "extension");

	while (xexten) {
		int proceed = 0;
		const char *cont = switch_xml_attr(xexten, "continue");
		const char *exten_name = switch_xml_attr(xexten, "name");

		if (!exten_name) {
			exten_name = "UNKNOWN";
		}

		switch_log_printf(SWITCH_CHANNEL_LOG_CLEAN, SWITCH_LOG_DEBUG,
						  "Chatplan: %s parsing [%s->%s] continue=%s\n",
						  to, context, exten_name, cont ? cont : "false");

		proceed = parse_exten(event, xexten, &extension);

		if (proceed && !switch_true(cont)) {
			break;
		}

		xexten = xexten->next;
	}

	switch_xml_free(xml);
	xml = NULL;

  done:
	switch_xml_free(xml);
	return extension;
}


static switch_status_t chat_send(switch_event_t *message_event)
								 {
	switch_status_t status = SWITCH_STATUS_BREAK;
	switch_event_t *exten;
	int forwards = 0;
	const char *var;

	var = switch_event_get_header(message_event, "max_forwards");

	if (!var) {
		forwards = 70;
	} else {
		forwards = atoi(var);

		if (forwards) {
			forwards--;
		}

		if (!forwards) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max forwards reached\n");
			DUMP_EVENT(message_event);
			return SWITCH_STATUS_FALSE;
		}
	}

	if (forwards) {
		switch_event_add_header(message_event, SWITCH_STACK_BOTTOM, "max_forwards", "%d", forwards);
	}

	if ((exten = chatplan_hunt(message_event))) {
		switch_event_header_t *hp;

		for (hp = exten->headers; hp; hp = hp->next) {
			status = switch_core_execute_chat_app(message_event, hp->name, hp->value);
			if (!SWITCH_READ_ACCEPTABLE(status)) {
				status = SWITCH_STATUS_SUCCESS;
				break;
			}
		}

		switch_event_destroy(&exten);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "SMS chatplan no actions found\n");
	}

	return status;

}


/**
 * @brief add@suy:2021-1-7 此函数解析sip message消息里会议控制相关xml并执行相关动作
 *
 * xml的必须包含conference或conferenceID标签
 */
static switch_status_t conference_control(switch_event_t *message_event, switch_xml_t xctl)
{
	switch_xml_t xconf, xtype, xaction, xuser, xparam;		// xml相关节点
	switch_status_t status = SWITCH_STATUS_SUCCESS;			// 返回状态
	switch_stream_handle_t stream = { 0 };					// api执行的输出流

	/* xml节点对应的变量 */
	char *conf = NULL, *action = NULL;
	char *real_action = NULL;
	char *user = NULL;
	char *param = NULL;
	switch_stream_handle_t param_stream = { 0 };			// 参数输入流
	switch_bool_t isvideo = SWITCH_TRUE;

	/* api命令输入流 */
	switch_stream_handle_t arg_stream = { 0 };

	/* xml标签中寻找会议号，会议号包含在conference或者conferenceID标签内 */
	if (!(xconf = switch_xml_child_ignorcase(xctl, "conference")) &&
		!(xconf = switch_xml_child_ignorcase(xctl, "conferenceID"))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't find conference!\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	conf = switch_strip_whitespace(xconf->txt); // 去除字符串两端空白符

	/* xml标签中寻找执行的动作标签 */
	if (!(xaction = switch_xml_child_ignorcase(xctl, "action"))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't find action!\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	action = switch_strip_whitespace(xaction->txt); // 去除字符串两端空白符

	SWITCH_STANDARD_STREAM(stream);

	/* 呼叫类动作(需要会议号、会议类型和成员列表，可以包含flag)，例如：
		<control>
			<conference>3000</conference>
			<type>video</type>
			<action>bgdial</action>
			<user>1011</user>							<-- 没有flag -->
			<user flags="mute">1012</user>				<-- 只含有一个flag -->
			<user flags="mute|mintwo">72110114</user>	<-- 含有多个flag -->
		<control>
	*/
	if (!strcasecmp(action, "bgdial")) {
		real_action = "bgdial";
	} else if (!strcasecmp(action, "bgdial")) {
		real_action = "dial";
	}

	if (real_action) {
		/* xml标签中寻找会议类型，有video和audio两种，默认为video */
		if ((xtype = switch_xml_child_ignorcase(xctl, "type"))) {
			if (!strcmp(xtype->txt, "audio")) {
				isvideo = SWITCH_FALSE;
			} else if (strcmp(xtype->txt, "video")) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Type is not audio or video!\n");
				switch_goto_status(SWITCH_STATUS_FALSE, done);
			}
		} else {
			isvideo = SWITCH_TRUE;
		}

		/* xml标签中寻找第一个成员的标签 */
		if (!(xuser = switch_xml_child_ignorcase(xctl, "user")) &&
			!(xuser = switch_xml_child_ignorcase(xctl, "userID"))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't find user!\n");
			switch_goto_status(SWITCH_STATUS_FALSE, done);
		}

		/* 遍历成员列表 */
		for (; xuser; xuser = xuser->next) {
			/* 判断是否含有flag */
			const char *flags = switch_xml_attr(xuser, "flags");
			switch_bool_t have_flags = !zstr(flags) && strcasecmp(flags, "none");

			user = switch_strip_whitespace(xuser->txt);

			/* 格式化写入命令 */
			SWITCH_STANDARD_STREAM(arg_stream);
			arg_stream.write_function(&arg_stream, "%s%s%s%s%s %s user/%s", conf,
									  isvideo?"@mcu":"",have_flags?"+flags{":"",
									  have_flags?flags:"", have_flags?"}":"", real_action, user);

			/* 执行命令 */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "EXCUTE API: conference %s\n", (char *) arg_stream.data);
			switch_api_execute("conference", (char *) arg_stream.data, NULL, &stream);

			switch_safe_free(user);
			switch_safe_free(arg_stream.data);
		}

		switch_goto_status(SWITCH_STATUS_SUCCESS, done);
	}

	/* 全局类动作(只需要会议号)，例如：
		<control>
			<conference>3000</conference>
			<action>exit</action>
		<control>
	*/
	if (!strcasecmp(action, "exit")) {
		real_action = "kick all";
	}

	/* 全局类动作的执行 */
	if (real_action) {
		/* 格式化写入命令 */
		SWITCH_STANDARD_STREAM(arg_stream);
		arg_stream.write_function(&arg_stream, "%s %s", conf, real_action);

		/* 执行命令 */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "EXCUTE API: conference %s\n", (char *) arg_stream.data);
		switch_api_execute("conference", (char *) arg_stream.data, NULL, &stream);

		switch_goto_status(SWITCH_STATUS_SUCCESS, done);
	}

	/* 全局类动作(需要会议号和全局参数)，例如：
		<control>
			<conference>3000</conference>
			<action>layout</action>
			<param>2x2</param>
		<control>
	*/
	if (!strcasecmp(action, "layout")) {
		real_action = "vid-layout";
	}

	if (real_action) {
		/* xml标签中寻找第一个全局参数的标签 */
		if (!(xparam = switch_xml_child_ignorcase(xctl, "param"))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't find param!\n");
			switch_goto_status(SWITCH_STATUS_FALSE, done);
		}

		/* 遍历参数列表，并写入全局参数流 */
		SWITCH_STANDARD_STREAM(param_stream);
		for (; xparam; xparam = xparam->next) {
			param = switch_strip_whitespace(xparam->txt);
			param_stream.write_function(&param_stream, "%s ", param);
			switch_safe_free(param);
		}
		/* 格式化写入命令 */
		SWITCH_STANDARD_STREAM(arg_stream);
		arg_stream.write_function(&arg_stream, "%s %s %s", conf, real_action, (char *) param_stream.data);

		/* 执行命令 */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "EXCUTE API: conference %s\n", (char *) arg_stream.data);
		switch_api_execute("conference", (char *) arg_stream.data, NULL, &stream);

		switch_goto_status(SWITCH_STATUS_SUCCESS, done);
	}

	/* 成员类动作(需要会议号和成员列表)，例如：
		<control>
			<conference>3000</conference>
			<action>kick</action>
			<user>1011</user>
		<control>
	*/
	if (!strcasecmp(action, "kick")) {
		real_action = "num-kick";
	} else if (!strcasecmp(action, "mute")) {
		real_action = "num-mute";
	} else if (!strcasecmp(action, "vmute")) {
		real_action = "num-vmute";
	} else if (!strcasecmp(action, "unmute")) {
		real_action = "num-unmute";
	} else if (!strcasecmp(action, "unvmute")) {
		real_action = "num-unvmute";
	}

	if (real_action) {
		/* xml标签中寻找第一个成员的标签 */
		if (!(xuser = switch_xml_child_ignorcase(xctl, "user")) &&
			!(xuser = switch_xml_child_ignorcase(xctl, "userID"))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't find user!\n");
			switch_goto_status(SWITCH_STATUS_FALSE, done);
		}

		/* 遍历成员列表 */
		for (; xuser; xuser = xuser->next) {
			user = switch_strip_whitespace(xuser->txt);

			/* 格式化写入命令 */
			SWITCH_STANDARD_STREAM(arg_stream);
			arg_stream.write_function(&arg_stream, "%s %s %s", conf, real_action, user);

			/* 执行命令 */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "EXCUTE API: conference %s\n", (char *) arg_stream.data);
			switch_api_execute("conference", (char *) arg_stream.data, NULL, &stream);

			switch_safe_free(user);
			switch_safe_free(arg_stream.data);
		}

		switch_goto_status(SWITCH_STATUS_SUCCESS, done);
	}

	/* 成员类动作(需要会议号、成员列表和参数)，例如：
		<control>
			<conference>3000</conference>
			<action>layer</action>
			<user param="1">1011</user>
			<user param="2">1012</user>
		<control>
	*/
	if (!strcasecmp(action, "layer")) {
		real_action = "num-layer";
	} else if (!strcasecmp(action, "banner")) {
		real_action = "num-banner";
	}

	if (real_action) {
		/* xml标签中寻找第一个成员的标签 */
		if (!(xuser = switch_xml_child_ignorcase(xctl, "user")) &&
			!(xuser = switch_xml_child_ignorcase(xctl, "userID"))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't find user!\n");
			switch_goto_status(SWITCH_STATUS_FALSE, done);
		}

		/* xml标签中寻找第一个全局参数的标签 */
		xparam = switch_xml_child_ignorcase(xctl, "param");

		/* 遍历参数列表，并写入参数流 */
		SWITCH_STANDARD_STREAM(param_stream);
		for (; xparam; xparam = xparam->next) {
			param = switch_strip_whitespace(xparam->txt);
			param_stream.write_function(&param_stream, "%s ", param);
			switch_safe_free(param);
		}

		/* 遍历成员列表 */
		for (; xuser; xuser = xuser->next) {
			/* 寻找用户参数 */
			const char *param_s = switch_xml_attr(xuser, "param");

			user = switch_strip_whitespace(xuser->txt);
			param = switch_strip_whitespace(param_s);

			/* 格式化写入命令 */
			SWITCH_STANDARD_STREAM(arg_stream);
			arg_stream.write_function(&arg_stream, "%s %s %s %s %s", conf, real_action, user, param /* 用户参数 */, (char *) param_stream.data /* 全局参数 */);

			/* 执行命令 */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "EXCUTE API: conference %s\n", (char *) arg_stream.data);
			switch_api_execute("conference", (char *) arg_stream.data, NULL, &stream);

			switch_safe_free(user);
			switch_safe_free(param);
			switch_safe_free(arg_stream.data);
		}

		switch_goto_status(SWITCH_STATUS_SUCCESS, done);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Cannot find action %s", action);
	switch_goto_status(SWITCH_STATUS_FALSE, done);

  done:
	switch_safe_free(conf);
	switch_safe_free(action);
	switch_safe_free(user);
	switch_safe_free(param);
	switch_safe_free(arg_stream.data);
	switch_safe_free(param_stream.data);
	switch_safe_free(stream.data);

	return status;
}

SWITCH_STANDARD_CHAT_APP(info_function)
{
	char *buf;
	int level = SWITCH_LOG_INFO;

	if (!zstr(data)) {
		level = switch_log_str2level(data);
	}

	switch_event_serialize(message, &buf, SWITCH_FALSE);
	switch_assert(buf);
	switch_log_printf(SWITCH_CHANNEL_LOG, level, "CHANNEL_DATA:\n%s\n", buf);
	free(buf);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_CHAT_APP(system_function)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Executing command: %s\n", data);
	if (switch_system(data, SWITCH_TRUE) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Failed to execute command: %s\n", data);
		return SWITCH_STATUS_FALSE;
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_CHAT_APP(stop_function)
{
	switch_set_flag(message, EF_NO_CHAT_EXEC);
	return SWITCH_STATUS_FALSE;
}

SWITCH_STANDARD_CHAT_APP(send_function)
{
	const char *dest_proto = data;

	if (zstr(dest_proto)) {
		dest_proto = switch_event_get_header(message, "dest_proto");
	}

	switch_event_add_header(message, SWITCH_STACK_BOTTOM, "skip_global_process", "true");

	switch_core_chat_send(dest_proto, message);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_CHAT_APP(set_function)
{
	char *var, *val;

	if (!data) return SWITCH_STATUS_SUCCESS;

	var = strdup(data);

	if (!var) return SWITCH_STATUS_SUCCESS;

	if ((val = strchr(var, '='))) {
		*val++ = '\0';
	}

	if (zstr(val)) {
		switch_event_del_header(message, var);
	} else {
		switch_event_add_header_string(message, SWITCH_STACK_BOTTOM, var, val);
	}

	free(var);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_CHAT_APP(unset_function)
{
	char *var;

	if (!data) return SWITCH_STATUS_SUCCESS;

	var = strdup(data);

	if (!var) return SWITCH_STATUS_SUCCESS;

	if (!zstr(var)) {
		switch_event_del_header(message, var);
	}

	free(var);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_CHAT_APP(fire_function)
{
	switch_event_t *fireme;

	switch_event_dup(&fireme, message);
	switch_event_fire(&fireme);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_CHAT_APP(reply_function)
{
	switch_event_t *reply;
	const char *proto = switch_event_get_header(message, "proto");

	if (proto) {
		switch_ivr_create_message_reply(&reply, message, SMS_CHAT_PROTO);

		if (!zstr(data)) {
			switch_event_set_body(reply, data);
		}

		switch_core_chat_deliver(proto, &reply);

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_SUCCESS;
}

/**
 * @brief add@suy:2021-1-4 此函数解析sip message消息里的cttcds命令并执行
 *
 * 可以在一个sip消息里携带多条命令，每条命令为单独的一行
 */
SWITCH_STANDARD_CHAT_APP(cmdctl_function)
{
	char *cmd_data, *cmd = NULL, *arg = NULL;
	char *pdata, *end;

	if (!data) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No cmd data\n");
		return SWITCH_STATUS_FALSE;
	}

	if (!(cmd_data = strdup(data))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't create cmd data\n");
		return SWITCH_STATUS_FALSE;
	}

	/* 遍历字符串的每一行(每一行就是一条命令)，例如：
		conference 3000 bgdial user/1011
		conference 3000 bgdial user/1012
	*/
	for (pdata = cmd_data; *pdata; ) {
		switch_stream_handle_t stream = { 0 };

		/* 跳过开始的空白符，确定命令字符串的开始位置 */
		while (isspace(*pdata)) {
			pdata++;
		}
		if (*pdata) {
			cmd = pdata;
		} else {
			break;
		}

		/* 遍历命令字符串，并确定命令字符串的结束位置 */
		while (*pdata && !isspace(*pdata)) {
			pdata++;
		}
		if (*pdata) {
			end = pdata;

			/* 跳过除了换行符的空白符，并根据是否遍历到换行符确定此行命令是否含有参数 */
			while (isspace(*pdata) && *pdata != '\n') {
				pdata++;
			}
			if (*pdata != '\n') {
				/* 包含参数，将参数遍历出来 */
				arg = pdata;
				while (*pdata && *pdata != '\n') {
					pdata++;
				}
				if (*pdata) {
					*pdata++ = '\0';
				}
			} else {
				/* 不包含参数 */
				arg = NULL;
			}

			*end = '\0';
		}

		SWITCH_STANDARD_STREAM(stream);

		/* 执行命令 */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "EXCUTE API: %s %s\n", cmd, arg);
		switch_api_execute(cmd, arg, NULL, &stream);

		switch_safe_free(stream.data);
	}

	free(cmd_data);

	return SWITCH_STATUS_SUCCESS;
}

/**
 * @brief add@suy:2021-1-6 此函数解析sip message消息里的xml并执行相应的动作
 *
 * xml的根元素必须为control
 */
SWITCH_STANDARD_CHAT_APP(xmlctl_function)
{
	char *xml_data = NULL;
	switch_xml_t xctl = NULL, xtype;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!data) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No XML data!\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	if (!(xml_data = strdup(data))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't create XML data!\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	/* 解析字符串中的xml结构 */
	if (!(xctl = switch_xml_parse_str_dynamic(xml_data, SWITCH_TRUE))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "XML ERROR!\n");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	/* 解析xml的根元素control */
	if (!xctl->name || strcasecmp(xctl->name, "control")) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "XML format error: Couldn't find root tag <Control>!");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

	/* 根据xml类型分发到指定的函数解析，目前只有会议控制 */
	if ((xtype = switch_xml_child_ignorcase(xctl, "conference")) ||
		(xtype = switch_xml_child_ignorcase(xctl, "conferenceID"))) {
		/* 会议控制 */
		status = conference_control(message, xctl);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "XML format error: Couldn't find correct type!");
		switch_goto_status(SWITCH_STATUS_FALSE, done);
	}

  done:
	switch_xml_free(xctl);
	switch_safe_free(xml_data);
	return status;
}

/* Macro expands to: switch_status_t mod_smsctl_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool) */
SWITCH_MODULE_LOAD_FUNCTION(mod_smsctl_load) /* change@suy:2021-1-3 */
{
	switch_chat_interface_t *chat_interface;
	switch_chat_application_interface_t *chat_app_interface;

	if (switch_event_reserve_subclass(MY_EVENT_DELIVERY_REPORT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", MY_EVENT_DELIVERY_REPORT);
		return SWITCH_STATUS_TERM;
	}

	if (switch_event_bind(modname, SWITCH_EVENT_CUSTOM, MY_EVENT_SEND_MESSAGE, event_handler, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_CHAT(chat_interface, SMS_CHAT_PROTO, chat_send);

	SWITCH_ADD_CHAT_APP(chat_app_interface, "info", "Display Call Info", "Display Call Info", info_function, "", SCAF_NONE);
	SWITCH_ADD_CHAT_APP(chat_app_interface, "reply", "reply to a message", "reply to a message", reply_function, "", SCAF_NONE);
	SWITCH_ADD_CHAT_APP(chat_app_interface, "stop", "stop execution", "stop execution", stop_function, "", SCAF_NONE);
	SWITCH_ADD_CHAT_APP(chat_app_interface, "set", "set a variable", "set a variable", set_function, "", SCAF_NONE);
	SWITCH_ADD_CHAT_APP(chat_app_interface, "unset", "unset a variable", "unset a variable", unset_function, "", SCAF_NONE);
	SWITCH_ADD_CHAT_APP(chat_app_interface, "send", "send the message as-is", "send the message as-is", send_function, "", SCAF_NONE);
	SWITCH_ADD_CHAT_APP(chat_app_interface, "fire", "fire the message", "fire the message", fire_function, "", SCAF_NONE);
	SWITCH_ADD_CHAT_APP(chat_app_interface, "system", "execute a system command", "execute a sytem command", system_function, "", SCAF_NONE);
	/* change@suy:2021-1-11 将cmdctl函数添加到chat_app中 */
	SWITCH_ADD_CHAT_APP(chat_app_interface, "cmdctl", "execute cttdcds commands", "execute cttdcds commands", cmdctl_function, "", SCAF_NONE);
	/* change@suy:2021-1-11 将xmlctl函数添加到chat_app中 */
	SWITCH_ADD_CHAT_APP(chat_app_interface, "xmlctl", "execute xml commands", "execute xml commands", xmlctl_function, "", SCAF_NONE);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_smsctl_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_smsctl_shutdown) /* change@suy:2021-1-3 */
{
	switch_event_unbind_callback(event_handler);

	switch_event_free_subclass(MY_EVENT_DELIVERY_REPORT);

	return SWITCH_STATUS_SUCCESS;
}




/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet
 */
