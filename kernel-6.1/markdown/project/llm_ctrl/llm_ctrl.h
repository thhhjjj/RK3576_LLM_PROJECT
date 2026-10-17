/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LLM_CTRL_H_
#define _LLM_CTRL_H_

#define LLM_CTRL_GENL_NAME		"llm_ctrl"
#define LLM_CTRL_GENL_VERSION		1
#define LLM_CTRL_MCGRP_EVENTS		"events"

enum llm_ctrl_cmds {
	LLM_CTRL_CMD_UNSPEC,
	LLM_CTRL_CMD_OVERTEMP,
	__LLM_CTRL_CMD_MAX,
};
#define LLM_CTRL_CMD_MAX (__LLM_CTRL_CMD_MAX - 1)

enum llm_ctrl_attrs {
	LLM_CTRL_A_UNSPEC,
	LLM_CTRL_A_TEMP_C,
	LLM_CTRL_A_THRESHOLD_C,
	__LLM_CTRL_A_MAX,
};
#define LLM_CTRL_A_MAX (__LLM_CTRL_A_MAX - 1)

/* Formal warn threshold (°C); below kernel critical 115°C. */
#define LLM_CTRL_DEFAULT_THRESHOLD_C	85

#endif /* _LLM_CTRL_H_ */
