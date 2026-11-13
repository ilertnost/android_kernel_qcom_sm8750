/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Quick Charge adapters.
 *
 * Quick Charge is negotiated by asking the charger to step its output up or
 * down, so unlike Power Delivery there is no capability list to read: the
 * driver names a voltage and the adapter either follows or does not.  The
 * driver that owns the charger registers here, and the generic adapter code
 * reaches Quick Charge through the protocol class.
 */

#ifndef __MCA_PROTOCOL_QC_H
#define __MCA_PROTOCOL_QC_H

#include <mca/protocol/protocol_class.h>
#include <linux/types.h>

/**
 * struct protocol_class_qc_ops - what the charger driver provides
 * @protocol_qc_get_qc_type:        which revision of Quick Charge was
 *                                  negotiated
 * @protocol_qc_set_volt:           ask the adapter for a voltage, in
 *                                  millivolts
 * @protocol_qc_set_volt_cmd:       step the adapter up or down by one
 *                                  increment
 * @protocol_qc3_check_class_type:  how much power a QC3+ adapter is rated for
 *
 * Every call takes the @data the charger driver registered.
 */
struct protocol_class_qc_ops {
	int (*protocol_qc_get_qc_type)(int *qc_type, void *data);
	int (*protocol_qc_set_volt)(void *data, int volt_mv);
	int (*protocol_qc_set_volt_cmd)(void *data, int hvdcp_cmd);
	int (*protocol_qc3_check_class_type)(int *class_type, void *data);
};

/*
 * A Quick Charge 3.0 adapter is tuned by asking it up or down one step at a
 * time; the step is the standard's, not the board's.
 */
#define QC3_STEP_SIZE			200

/*
 * The fixed voltages a Quick Charge 2.0 adapter is commanded to, as the
 * numbers the charger firmware takes.
 */
#define QC2_FORCE_5V			2
#define QC2_FORCE_9V			1
#define QC2_FORCE_12V			0

/* And the two commands that walk a Quick Charge 3.0 adapter one step. */
#define QC3_SINGLE_INCREMENT		0x20
#define QC3_SINGLE_DECREMENT		0x10

int protocol_class_qc_register_ops(enum adatper_protocol protocol,
				   const struct protocol_class_qc_ops *ops,
				   void *data);

int protocol_class_qc_set_volt(enum adatper_protocol protocol, int volt);
int protocol_class_qc_set_volt_cmd(enum adatper_protocol protocol,
				   int hvdcp_cmd);
int protocol_class_qc_get_qc_type(enum adatper_protocol protocol, int *qc_type);
int protocol_class_qc3_check_class_type(enum adatper_protocol protocol,
					int *class_type);

#endif /* __MCA_PROTOCOL_QC_H */
