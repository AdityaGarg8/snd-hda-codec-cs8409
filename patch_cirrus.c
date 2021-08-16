// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HD audio interface patch for Cirrus Logic CS420x chip
 *
 * Copyright (c) 2009 Takashi Iwai <tiwai@suse.de>
 */

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <sound/core.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <sound/tlv.h>
#include <sound/hda_codec.h>
#include <linux/ctype.h>
#include <linux/timer.h>
#include "hda_local.h"
#include "hda_auto_parser.h"
#include "hda_jack.h"
#include "hda_generic.h"

#include <linux/bitops.h>


// define some explicit debugging print functions
// under flag control so can be easily turned off


#ifdef MYSOUNDDEBUGFULL
#define mycodec_info(codec, fmt, args...) \
        dev_info(hda_codec_dev(codec), fmt, ##args)
#define mycodec_i2c_info(codec, fmt, args...) \
        dev_info(hda_codec_dev(codec), fmt, ##args)
#define mycodec_dbg(codec, fmt, args...) \
        dev_info(hda_codec_dev(codec), fmt, ##args)
#define myprintk_dbg(fmt, args...) \
        printk(fmt, ##args)
#define myprintk(fmt, args...) \
        printk(fmt, ##args)
#else
#define mycodec_dbg(...)
#define myprintk_dbg(...)
#ifdef MYSOUNDDEBUG
#define mycodec_info(codec, fmt, args...) \
        dev_info(hda_codec_dev(codec), fmt, ##args)
#define mycodec_i2c_info(codec, fmt, args...) \
        dev_info(hda_codec_dev(codec), fmt, ##args)
#define myprintk(fmt, args...) \
        printk(fmt, ##args)
#else
#define mycodec_info(...)
#define mycodec_i2c_info(...)
#define myprintk(...)
#endif
#endif


/*
 */


#define CS42L42_HP_CH     (2U)
#define CS42L42_HS_MIC_CH (1U)

struct unsol_item {
        struct list_head list;
        unsigned int idx;
        unsigned int res;
};

struct cs_spec {
	struct hda_gen_spec gen;

	unsigned int gpio_mask;
	unsigned int gpio_dir;
	unsigned int gpio_data;
	unsigned int gpio_eapd_hp; /* EAPD GPIO bit for headphones */
	unsigned int gpio_eapd_speaker; /* EAPD GPIO bit for speakers */

	/* CS421x */
	unsigned int spdif_detect:1;
	unsigned int spdif_present:1;
	unsigned int sense_b:1;
	hda_nid_t vendor_nid;

        /* digital beep */
        hda_nid_t beep_nid;

	/* for MBP SPDIF control */
	int (*spdif_sw_put)(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol);

	unsigned int cs42l42_hp_jack_in:1;
	unsigned int cs42l42_mic_jack_in:1;
	unsigned int cs42l42_volume_init:1;
	char cs42l42_hp_volume[CS42L42_HP_CH];
	char cs42l42_hs_mic_volume[CS42L42_HS_MIC_CH];

	struct mutex cs8409_i2c_mux;

	/* verb exec op override */
	int (*exec_verb)(struct hdac_device *dev, unsigned int cmd,
				 unsigned int flags, unsigned int *res);

	// so it appears we have "concurrency" in the linux HDA code
	// in that if unsolicited responses occur which perform extensive verbs
	// the hda verbs are intermixed with eg extensive start playback verbs
	// on OSX we appear to have blocks of verbs during which unsolicited responses
	// are logged but the unsolicited verbs occur after the verb block
	// this flag is used to flag such verb blocks and the list will store the
	// responses
	// we use a pre-allocated list - if we have more than 10 outstanding unsols
	// we will drop
	// not clear if mutexes would be the way to go
	int block_unsol;
	struct list_head unsol_list;
	struct unsol_item unsol_items_prealloc[10];
	int unsol_items_prealloc_used[10];

	// add in specific nids for the intmike and linein as they seem to swap
	// between macbook pros (14,3) and imacs (18,3)
	int intmike_nid;
	int linein_nid;
	int intmike_adc_nid;
	int linein_amp_nid;

	// new item to deal with jack presence as Apple seems to have barfed
	// the HDA spec by using a separate headphone chip
	int jack_present;

	// save the type of headphone connected
	int headset_type;

	// if headphone has mike or not
	int have_mike;

	// if headphone has buttons or not
	int have_buttons;

	// set when playing for plug/unplug events while playing
	int playing;

	// set when capturing for plug/unplug events while capturing
	int capturing;

	// changing coding - OSX sets up the format on plugin
	// then does some minimal setup when start play
	// initial coding delayed any format setup till actually play
	// this works for no mike but not for mike - we need to initialize
	// the mike on plugin
	// this flag will be set when we have done the format setup
	// so know if need to do it on play or not
	// now need 2 flags - one for play and one for capture
	int headset_play_format_setup_needed;
	int headset_capture_format_setup_needed;

	int headset_presetup_done;


	int use_data;


	// this is new item for dealing with headset plugins
	// so can distinguish which phase we are in if have multiple interrupts
	// not really used now have analyzed interrupts properly
	int headset_phase;

	// another dirty hack item to manage the different headset enable codes
	int headset_enable;

	int play_init;
	int capture_init;

	// new item to limit times we redo unmute/play
	struct timespec64 last_play_time;
	// record the first play time - we have a problem there
	// some initial plays that I dont understand - so skip any setup
	// till sometime after the first play
	struct timespec64 first_play_time;
};

/* available models with CS420x */
enum {
	CS420X_MBP53,
	CS420X_MBP55,
	CS420X_IMAC27,
	CS420X_GPIO_13,
	CS420X_GPIO_23,
	CS420X_MBP101,
	CS420X_MBP81,
	CS420X_MBA42,
	CS420X_AUTO,
	/* aliases */
	CS420X_IMAC27_122 = CS420X_GPIO_23,
	CS420X_APPLE = CS420X_GPIO_13,
};

/* CS421x boards */
enum {
	CS421X_CDB4210,
	CS421X_SENSE_B,
	CS421X_STUMPY,
};

/* Vendor-specific processing widget */
#define CS420X_VENDOR_NID	0x11
#define CS_DIG_OUT1_PIN_NID	0x10
#define CS_DIG_OUT2_PIN_NID	0x15
#define CS_DMIC1_PIN_NID	0x0e
#define CS_DMIC2_PIN_NID	0x12

/* coef indices */
#define IDX_SPDIF_STAT		0x0000
#define IDX_SPDIF_CTL		0x0001
#define IDX_ADC_CFG		0x0002
/* SZC bitmask, 4 modes below:
 * 0 = immediate,
 * 1 = digital immediate, analog zero-cross
 * 2 = digtail & analog soft-ramp
 * 3 = digital soft-ramp, analog zero-cross
 */
#define   CS_COEF_ADC_SZC_MASK		(3 << 0)
#define   CS_COEF_ADC_MIC_SZC_MODE	(3 << 0) /* SZC setup for mic */
#define   CS_COEF_ADC_LI_SZC_MODE	(3 << 0) /* SZC setup for line-in */
/* PGA mode: 0 = differential, 1 = signle-ended */
#define   CS_COEF_ADC_MIC_PGA_MODE	(1 << 5) /* PGA setup for mic */
#define   CS_COEF_ADC_LI_PGA_MODE	(1 << 6) /* PGA setup for line-in */
#define IDX_DAC_CFG		0x0003
/* SZC bitmask, 4 modes below:
 * 0 = Immediate
 * 1 = zero-cross
 * 2 = soft-ramp
 * 3 = soft-ramp on zero-cross
 */
#define   CS_COEF_DAC_HP_SZC_MODE	(3 << 0) /* nid 0x02 */
#define   CS_COEF_DAC_LO_SZC_MODE	(3 << 2) /* nid 0x03 */
#define   CS_COEF_DAC_SPK_SZC_MODE	(3 << 4) /* nid 0x04 */

#define IDX_BEEP_CFG		0x0004
/* 0x0008 - test reg key */
/* 0x0009 - 0x0014 -> 12 test regs */
/* 0x0015 - visibility reg */

/* Cirrus Logic CS4208 */
#define CS4208_VENDOR_NID	0x24

/*
 * Cirrus Logic CS4210
 *
 * 1 DAC => HP(sense) / Speakers,
 * 1 ADC <= LineIn(sense) / MicIn / DMicIn,
 * 1 SPDIF OUT => SPDIF Trasmitter(sense)
 */
#define CS4210_DAC_NID		0x02
#define CS4210_ADC_NID		0x03
#define CS4210_VENDOR_NID	0x0B
#define CS421X_DMIC_PIN_NID	0x09 /* Port E */
#define CS421X_SPDIF_PIN_NID	0x0A /* Port H */

#define CS421X_IDX_DEV_CFG	0x01
#define CS421X_IDX_ADC_CFG	0x02
#define CS421X_IDX_DAC_CFG	0x03
#define CS421X_IDX_SPK_CTL	0x04

/* Cirrus Logic CS4213 is like CS4210 but does not have SPDIF input/output */
#define CS4213_VENDOR_NID	0x09


/* CS8409 */
#define CS8409_IDX_DEV_CFG     0x01
#define CS8409_VENDOR_NID      0x47
#define CS8409_BEEP_NID        0x46


static inline int cs_vendor_coef_get(struct hda_codec *codec, unsigned int idx)
{
	struct cs_spec *spec = codec->spec;

	snd_hda_codec_write(codec, spec->vendor_nid, 0,
			    AC_VERB_SET_COEF_INDEX, idx);
	return snd_hda_codec_read(codec, spec->vendor_nid, 0,
				  AC_VERB_GET_PROC_COEF, 0);
}

static inline void cs_vendor_coef_set(struct hda_codec *codec, unsigned int idx,
				      unsigned int coef)
{
	struct cs_spec *spec = codec->spec;

	snd_hda_codec_write(codec, spec->vendor_nid, 0,
			    AC_VERB_SET_COEF_INDEX, idx);
	snd_hda_codec_write(codec, spec->vendor_nid, 0,
			    AC_VERB_SET_PROC_COEF, coef);
}

/*
 * auto-mute and auto-mic switching
 * CS421x auto-output redirecting
 * HP/SPK/SPDIF
 */

static void cs_automute(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	/* mute HPs if spdif jack (SENSE_B) is present */
	spec->gen.master_mute = !!(spec->spdif_present && spec->sense_b);

	snd_hda_gen_update_outputs(codec);

	if (spec->gpio_eapd_hp || spec->gpio_eapd_speaker) {
		if (spec->gen.automute_speaker)
			spec->gpio_data = spec->gen.hp_jack_present ?
				spec->gpio_eapd_hp : spec->gpio_eapd_speaker;
		else
			spec->gpio_data =
				spec->gpio_eapd_hp | spec->gpio_eapd_speaker;
		snd_hda_codec_write(codec, 0x01, 0,
				    AC_VERB_SET_GPIO_DATA, spec->gpio_data);
	}
}

static bool is_active_pin(struct hda_codec *codec, hda_nid_t nid)
{
	unsigned int val;

	val = snd_hda_codec_get_pincfg(codec, nid);
	return (get_defcfg_connect(val) != AC_JACK_PORT_NONE);
}

static void init_input_coef(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	unsigned int coef;

	/* CS420x has multiple ADC, CS421x has single ADC */
	if (spec->vendor_nid == CS420X_VENDOR_NID) {
		coef = cs_vendor_coef_get(codec, IDX_BEEP_CFG);
		if (is_active_pin(codec, CS_DMIC2_PIN_NID))
			coef |= 1 << 4; /* DMIC2 2 chan on, GPIO1 off */
		if (is_active_pin(codec, CS_DMIC1_PIN_NID))
			coef |= 1 << 3; /* DMIC1 2 chan on, GPIO0 off
					 * No effect if SPDIF_OUT2 is
					 * selected in IDX_SPDIF_CTL.
					 */

		cs_vendor_coef_set(codec, IDX_BEEP_CFG, coef);
	}
}

static const struct hda_verb cs_coef_init_verbs[] = {
	{0x11, AC_VERB_SET_PROC_STATE, 1},
	{0x11, AC_VERB_SET_COEF_INDEX, IDX_DAC_CFG},
	{0x11, AC_VERB_SET_PROC_COEF,
	 (0x002a /* DAC1/2/3 SZCMode Soft Ramp */
	  | 0x0040 /* Mute DACs on FIFO error */
	  | 0x1000 /* Enable DACs High Pass Filter */
	  | 0x0400 /* Disable Coefficient Auto increment */
	  )},
	/* ADC1/2 - Digital and Analog Soft Ramp */
	{0x11, AC_VERB_SET_COEF_INDEX, IDX_ADC_CFG},
	{0x11, AC_VERB_SET_PROC_COEF, 0x000a},
	/* Beep */
	{0x11, AC_VERB_SET_COEF_INDEX, IDX_BEEP_CFG},
	{0x11, AC_VERB_SET_PROC_COEF, 0x0007}, /* Enable Beep thru DAC1/2/3 */

	{} /* terminator */
};

static const struct hda_verb cs4208_coef_init_verbs[] = {
	{0x01, AC_VERB_SET_POWER_STATE, 0x00}, /* AFG: D0 */
	{0x24, AC_VERB_SET_PROC_STATE, 0x01},  /* VPW: processing on */
	{0x24, AC_VERB_SET_COEF_INDEX, 0x0033},
	{0x24, AC_VERB_SET_PROC_COEF, 0x0001}, /* A1 ICS */
	{0x24, AC_VERB_SET_COEF_INDEX, 0x0034},
	{0x24, AC_VERB_SET_PROC_COEF, 0x1C01}, /* A1 Enable, A Thresh = 300mV */
	{} /* terminator */
};

/* Errata: CS4207 rev C0/C1/C2 Silicon
 *
 * http://www.cirrus.com/en/pubs/errata/ER880C3.pdf
 *
 * 6. At high temperature (TA > +85°C), the digital supply current (IVD)
 * may be excessive (up to an additional 200 μA), which is most easily
 * observed while the part is being held in reset (RESET# active low).
 *
 * Root Cause: At initial powerup of the device, the logic that drives
 * the clock and write enable to the S/PDIF SRC RAMs is not properly
 * initialized.
 * Certain random patterns will cause a steady leakage current in those
 * RAM cells. The issue will resolve once the SRCs are used (turned on).
 *
 * Workaround: The following verb sequence briefly turns on the S/PDIF SRC
 * blocks, which will alleviate the issue.
 */

static const struct hda_verb cs_errata_init_verbs[] = {
	{0x01, AC_VERB_SET_POWER_STATE, 0x00}, /* AFG: D0 */
	{0x11, AC_VERB_SET_PROC_STATE, 0x01},  /* VPW: processing on */

	{0x11, AC_VERB_SET_COEF_INDEX, 0x0008},
	{0x11, AC_VERB_SET_PROC_COEF, 0x9999},
	{0x11, AC_VERB_SET_COEF_INDEX, 0x0017},
	{0x11, AC_VERB_SET_PROC_COEF, 0xa412},
	{0x11, AC_VERB_SET_COEF_INDEX, 0x0001},
	{0x11, AC_VERB_SET_PROC_COEF, 0x0009},

	{0x07, AC_VERB_SET_POWER_STATE, 0x00}, /* S/PDIF Rx: D0 */
	{0x08, AC_VERB_SET_POWER_STATE, 0x00}, /* S/PDIF Tx: D0 */

	{0x11, AC_VERB_SET_COEF_INDEX, 0x0017},
	{0x11, AC_VERB_SET_PROC_COEF, 0x2412},
	{0x11, AC_VERB_SET_COEF_INDEX, 0x0008},
	{0x11, AC_VERB_SET_PROC_COEF, 0x0000},
	{0x11, AC_VERB_SET_COEF_INDEX, 0x0001},
	{0x11, AC_VERB_SET_PROC_COEF, 0x0008},
	{0x11, AC_VERB_SET_PROC_STATE, 0x00},
	{} /* terminator */
};

/* SPDIF setup */
static void init_digital_coef(struct hda_codec *codec)
{
	unsigned int coef;

	coef = 0x0002; /* SRC_MUTE soft-mute on SPDIF (if no lock) */
	coef |= 0x0008; /* Replace with mute on error */
	if (is_active_pin(codec, CS_DIG_OUT2_PIN_NID))
		coef |= 0x4000; /* RX to TX1 or TX2 Loopthru / SPDIF2
				 * SPDIF_OUT2 is shared with GPIO1 and
				 * DMIC_SDA2.
				 */
	cs_vendor_coef_set(codec, IDX_SPDIF_CTL, coef);
}

static int cs_init(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	codec_dbg(codec, "cs_init enter\n");

	if (spec->vendor_nid == CS420X_VENDOR_NID) {
		/* init_verb sequence for C0/C1/C2 errata*/
		snd_hda_sequence_write(codec, cs_errata_init_verbs);
		snd_hda_sequence_write(codec, cs_coef_init_verbs);
	} else if (spec->vendor_nid == CS4208_VENDOR_NID) {
		snd_hda_sequence_write(codec, cs4208_coef_init_verbs);
	}

	snd_hda_gen_init(codec);

	if (spec->gpio_mask) {
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_MASK,
				    spec->gpio_mask);
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DIRECTION,
				    spec->gpio_dir);
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DATA,
				    spec->gpio_data);
	}

	if (spec->vendor_nid == CS420X_VENDOR_NID) {
		init_input_coef(codec);
		init_digital_coef(codec);
	}

	codec_dbg(codec, "cs_init exit\n");

	return 0;
}

static int cs_build_controls(struct hda_codec *codec)
{
	int err;

	err = snd_hda_gen_build_controls(codec);
	if (err < 0)
		return err;
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_BUILD);
	return 0;
}

#define cs_free		snd_hda_gen_free

// attempt at an explicit setup ie not generic
//#include "patch_cirrus_explicit.h"


static const struct hda_codec_ops cs_patch_ops = {
	.build_controls = cs_build_controls,
	.build_pcms = snd_hda_gen_build_pcms,
	.init = cs_init,
	.free = cs_free,
	.unsol_event = snd_hda_jack_unsol_event,
};

static int cs_parse_auto_config(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	int err;
	int i;

	err = snd_hda_parse_pin_defcfg(codec, &spec->gen.autocfg, NULL, 0);
	if (err < 0)
		return err;

	err = snd_hda_gen_parse_auto_config(codec, &spec->gen.autocfg);
	if (err < 0)
		return err;

	/* keep the ADCs powered up when it's dynamically switchable */
	if (spec->gen.dyn_adc_switch) {
		unsigned int done = 0;

		for (i = 0; i < spec->gen.input_mux.num_items; i++) {
			int idx = spec->gen.dyn_adc_idx[i];

			if (done & (1 << idx))
				continue;
			snd_hda_gen_fix_pin_power(codec,
						  spec->gen.adc_nids[idx]);
			done |= 1 << idx;
		}
	}

	return 0;
}

static const struct hda_model_fixup cs420x_models[] = {
	{ .id = CS420X_MBP53, .name = "mbp53" },
	{ .id = CS420X_MBP55, .name = "mbp55" },
	{ .id = CS420X_IMAC27, .name = "imac27" },
	{ .id = CS420X_IMAC27_122, .name = "imac27_122" },
	{ .id = CS420X_APPLE, .name = "apple" },
	{ .id = CS420X_MBP101, .name = "mbp101" },
	{ .id = CS420X_MBP81, .name = "mbp81" },
	{ .id = CS420X_MBA42, .name = "mba42" },
	{}
};

static const struct snd_pci_quirk cs420x_fixup_tbl[] = {
	SND_PCI_QUIRK(0x10de, 0x0ac0, "MacBookPro 5,3", CS420X_MBP53),
	SND_PCI_QUIRK(0x10de, 0x0d94, "MacBookAir 3,1(2)", CS420X_MBP55),
	SND_PCI_QUIRK(0x10de, 0xcb79, "MacBookPro 5,5", CS420X_MBP55),
	SND_PCI_QUIRK(0x10de, 0xcb89, "MacBookPro 7,1", CS420X_MBP55),
	/* this conflicts with too many other models */
	/*SND_PCI_QUIRK(0x8086, 0x7270, "IMac 27 Inch", CS420X_IMAC27),*/

	/* codec SSID */
	SND_PCI_QUIRK(0x106b, 0x0600, "iMac 14,1", CS420X_IMAC27_122),
	SND_PCI_QUIRK(0x106b, 0x1c00, "MacBookPro 8,1", CS420X_MBP81),
	SND_PCI_QUIRK(0x106b, 0x2000, "iMac 12,2", CS420X_IMAC27_122),
	SND_PCI_QUIRK(0x106b, 0x2800, "MacBookPro 10,1", CS420X_MBP101),
	SND_PCI_QUIRK(0x106b, 0x5600, "MacBookAir 5,2", CS420X_MBP81),
	SND_PCI_QUIRK(0x106b, 0x5b00, "MacBookAir 4,2", CS420X_MBA42),
	SND_PCI_QUIRK_VENDOR(0x106b, "Apple", CS420X_APPLE),
	{} /* terminator */
};

static const struct hda_pintbl mbp53_pincfgs[] = {
	{ 0x09, 0x012b4050 },
	{ 0x0a, 0x90100141 },
	{ 0x0b, 0x90100140 },
	{ 0x0c, 0x018b3020 },
	{ 0x0d, 0x90a00110 },
	{ 0x0e, 0x400000f0 },
	{ 0x0f, 0x01cbe030 },
	{ 0x10, 0x014be060 },
	{ 0x12, 0x400000f0 },
	{ 0x15, 0x400000f0 },
	{} /* terminator */
};

static const struct hda_pintbl mbp55_pincfgs[] = {
	{ 0x09, 0x012b4030 },
	{ 0x0a, 0x90100121 },
	{ 0x0b, 0x90100120 },
	{ 0x0c, 0x400000f0 },
	{ 0x0d, 0x90a00110 },
	{ 0x0e, 0x400000f0 },
	{ 0x0f, 0x400000f0 },
	{ 0x10, 0x014be040 },
	{ 0x12, 0x400000f0 },
	{ 0x15, 0x400000f0 },
	{} /* terminator */
};

static const struct hda_pintbl imac27_pincfgs[] = {
	{ 0x09, 0x012b4050 },
	{ 0x0a, 0x90100140 },
	{ 0x0b, 0x90100142 },
	{ 0x0c, 0x018b3020 },
	{ 0x0d, 0x90a00110 },
	{ 0x0e, 0x400000f0 },
	{ 0x0f, 0x01cbe030 },
	{ 0x10, 0x014be060 },
	{ 0x12, 0x01ab9070 },
	{ 0x15, 0x400000f0 },
	{} /* terminator */
};

static const struct hda_pintbl mbp101_pincfgs[] = {
	{ 0x0d, 0x40ab90f0 },
	{ 0x0e, 0x90a600f0 },
	{ 0x12, 0x50a600f0 },
	{} /* terminator */
};

static const struct hda_pintbl mba42_pincfgs[] = {
	{ 0x09, 0x012b4030 }, /* HP */
	{ 0x0a, 0x400000f0 },
	{ 0x0b, 0x90100120 }, /* speaker */
	{ 0x0c, 0x400000f0 },
	{ 0x0d, 0x90a00110 }, /* mic */
	{ 0x0e, 0x400000f0 },
	{ 0x0f, 0x400000f0 },
	{ 0x10, 0x400000f0 },
	{ 0x12, 0x400000f0 },
	{ 0x15, 0x400000f0 },
	{} /* terminator */
};

static const struct hda_pintbl mba6_pincfgs[] = {
	{ 0x10, 0x032120f0 }, /* HP */
	{ 0x11, 0x500000f0 },
	{ 0x12, 0x90100010 }, /* Speaker */
	{ 0x13, 0x500000f0 },
	{ 0x14, 0x500000f0 },
	{ 0x15, 0x770000f0 },
	{ 0x16, 0x770000f0 },
	{ 0x17, 0x430000f0 },
	{ 0x18, 0x43ab9030 }, /* Mic */
	{ 0x19, 0x770000f0 },
	{ 0x1a, 0x770000f0 },
	{ 0x1b, 0x770000f0 },
	{ 0x1c, 0x90a00090 },
	{ 0x1d, 0x500000f0 },
	{ 0x1e, 0x500000f0 },
	{ 0x1f, 0x500000f0 },
	{ 0x20, 0x500000f0 },
	{ 0x21, 0x430000f0 },
	{ 0x22, 0x430000f0 },
	{} /* terminator */
};

static void cs420x_fixup_gpio_13(struct hda_codec *codec,
				 const struct hda_fixup *fix, int action)
{
	if (action == HDA_FIXUP_ACT_PRE_PROBE) {
		struct cs_spec *spec = codec->spec;

		spec->gpio_eapd_hp = 2; /* GPIO1 = headphones */
		spec->gpio_eapd_speaker = 8; /* GPIO3 = speakers */
		spec->gpio_mask = spec->gpio_dir =
			spec->gpio_eapd_hp | spec->gpio_eapd_speaker;
	}
}

static void cs420x_fixup_gpio_23(struct hda_codec *codec,
				 const struct hda_fixup *fix, int action)
{
	if (action == HDA_FIXUP_ACT_PRE_PROBE) {
		struct cs_spec *spec = codec->spec;

		spec->gpio_eapd_hp = 4; /* GPIO2 = headphones */
		spec->gpio_eapd_speaker = 8; /* GPIO3 = speakers */
		spec->gpio_mask = spec->gpio_dir =
			spec->gpio_eapd_hp | spec->gpio_eapd_speaker;
	}
}

static const struct hda_fixup cs420x_fixups[] = {
	[CS420X_MBP53] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = mbp53_pincfgs,
		.chained = true,
		.chain_id = CS420X_APPLE,
	},
	[CS420X_MBP55] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = mbp55_pincfgs,
		.chained = true,
		.chain_id = CS420X_GPIO_13,
	},
	[CS420X_IMAC27] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = imac27_pincfgs,
		.chained = true,
		.chain_id = CS420X_GPIO_13,
	},
	[CS420X_GPIO_13] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs420x_fixup_gpio_13,
	},
	[CS420X_GPIO_23] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs420x_fixup_gpio_23,
	},
	[CS420X_MBP101] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = mbp101_pincfgs,
		.chained = true,
		.chain_id = CS420X_GPIO_13,
	},
	[CS420X_MBP81] = {
		.type = HDA_FIXUP_VERBS,
		.v.verbs = (const struct hda_verb[]) {
			/* internal mic ADC2: right only, single ended */
			{0x11, AC_VERB_SET_COEF_INDEX, IDX_ADC_CFG},
			{0x11, AC_VERB_SET_PROC_COEF, 0x102a},
			{}
		},
		.chained = true,
		.chain_id = CS420X_GPIO_13,
	},
	[CS420X_MBA42] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = mba42_pincfgs,
		.chained = true,
		.chain_id = CS420X_GPIO_13,
	},
};

static struct cs_spec *cs_alloc_spec(struct hda_codec *codec, int vendor_nid)
{
	struct cs_spec *spec;

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return NULL;
	codec->spec = spec;
	spec->vendor_nid = vendor_nid;
	codec->power_save_node = 1;
	snd_hda_gen_spec_init(&spec->gen);

	return spec;
}

static int patch_cs420x(struct hda_codec *codec)
{
	struct cs_spec *spec;
	int err;

	spec = cs_alloc_spec(codec, CS420X_VENDOR_NID);
	if (!spec)
		return -ENOMEM;

	codec->patch_ops = cs_patch_ops;
	spec->gen.automute_hook = cs_automute;
	codec->single_adc_amp = 1;

	snd_hda_pick_fixup(codec, cs420x_models, cs420x_fixup_tbl,
			   cs420x_fixups);
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PRE_PROBE);

	err = cs_parse_auto_config(codec);
	if (err < 0)
		goto error;

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PROBE);

	return 0;

 error:
	cs_free(codec);
	return err;
}

/*
 * CS4208 support:
 * Its layout is no longer compatible with CS4206/CS4207
 */
enum {
	CS4208_MAC_AUTO,
	CS4208_MBA6,
	CS4208_MBP11,
	CS4208_MACMINI,
	CS4208_GPIO0,
};

static const struct hda_model_fixup cs4208_models[] = {
	{ .id = CS4208_GPIO0, .name = "gpio0" },
	{ .id = CS4208_MBA6, .name = "mba6" },
	{ .id = CS4208_MBP11, .name = "mbp11" },
	{ .id = CS4208_MACMINI, .name = "macmini" },
	{}
};

static const struct snd_pci_quirk cs4208_fixup_tbl[] = {
	SND_PCI_QUIRK_VENDOR(0x106b, "Apple", CS4208_MAC_AUTO),
	{} /* terminator */
};

/* codec SSID matching */
static const struct snd_pci_quirk cs4208_mac_fixup_tbl[] = {
	SND_PCI_QUIRK(0x106b, 0x5e00, "MacBookPro 11,2", CS4208_MBP11),
	SND_PCI_QUIRK(0x106b, 0x6c00, "MacMini 7,1", CS4208_MACMINI),
	SND_PCI_QUIRK(0x106b, 0x7100, "MacBookAir 6,1", CS4208_MBA6),
	SND_PCI_QUIRK(0x106b, 0x7200, "MacBookAir 6,2", CS4208_MBA6),
	SND_PCI_QUIRK(0x106b, 0x7b00, "MacBookPro 12,1", CS4208_MBP11),
	{} /* terminator */
};

static void cs4208_fixup_gpio0(struct hda_codec *codec,
			       const struct hda_fixup *fix, int action)
{
	if (action == HDA_FIXUP_ACT_PRE_PROBE) {
		struct cs_spec *spec = codec->spec;

		spec->gpio_eapd_hp = 0;
		spec->gpio_eapd_speaker = 1;
		spec->gpio_mask = spec->gpio_dir =
			spec->gpio_eapd_hp | spec->gpio_eapd_speaker;
	}
}

static const struct hda_fixup cs4208_fixups[];

/* remap the fixup from codec SSID and apply it */
static void cs4208_fixup_mac(struct hda_codec *codec,
			     const struct hda_fixup *fix, int action)
{
	if (action != HDA_FIXUP_ACT_PRE_PROBE)
		return;

	codec->fixup_id = HDA_FIXUP_ID_NOT_SET;
	snd_hda_pick_fixup(codec, NULL, cs4208_mac_fixup_tbl, cs4208_fixups);
	if (codec->fixup_id == HDA_FIXUP_ID_NOT_SET)
		codec->fixup_id = CS4208_GPIO0; /* default fixup */
	snd_hda_apply_fixup(codec, action);
}

/* MacMini 7,1 has the inverted jack detection */
static void cs4208_fixup_macmini(struct hda_codec *codec,
				 const struct hda_fixup *fix, int action)
{
	static const struct hda_pintbl pincfgs[] = {
		{ 0x18, 0x00ab9150 }, /* mic (audio-in) jack: disable detect */
		{ 0x21, 0x004be140 }, /* SPDIF: disable detect */
		{ }
	};

	if (action == HDA_FIXUP_ACT_PRE_PROBE) {
		/* HP pin (0x10) has an inverted detection */
		codec->inv_jack_detect = 1;
		/* disable the bogus Mic and SPDIF jack detections */
		snd_hda_apply_pincfgs(codec, pincfgs);
	}
}

static int cs4208_spdif_sw_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cs_spec *spec = codec->spec;
	hda_nid_t pin = spec->gen.autocfg.dig_out_pins[0];
	int pinctl = ucontrol->value.integer.value[0] ? PIN_OUT : 0;

	snd_hda_set_pin_ctl_cache(codec, pin, pinctl);
	return spec->spdif_sw_put(kcontrol, ucontrol);
}

/* hook the SPDIF switch */
static void cs4208_fixup_spdif_switch(struct hda_codec *codec,
				      const struct hda_fixup *fix, int action)
{
	if (action == HDA_FIXUP_ACT_BUILD) {
		struct cs_spec *spec = codec->spec;
		struct snd_kcontrol *kctl;

		if (!spec->gen.autocfg.dig_out_pins[0])
			return;
		kctl = snd_hda_find_mixer_ctl(codec, "IEC958 Playback Switch");
		if (!kctl)
			return;
		spec->spdif_sw_put = kctl->put;
		kctl->put = cs4208_spdif_sw_put;
	}
}

static const struct hda_fixup cs4208_fixups[] = {
	[CS4208_MBA6] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = mba6_pincfgs,
		.chained = true,
		.chain_id = CS4208_GPIO0,
	},
	[CS4208_MBP11] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs4208_fixup_spdif_switch,
		.chained = true,
		.chain_id = CS4208_GPIO0,
	},
	[CS4208_MACMINI] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs4208_fixup_macmini,
		.chained = true,
		.chain_id = CS4208_GPIO0,
	},
	[CS4208_GPIO0] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs4208_fixup_gpio0,
	},
	[CS4208_MAC_AUTO] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs4208_fixup_mac,
	},
};

/* correct the 0dB offset of input pins */
static void cs4208_fix_amp_caps(struct hda_codec *codec, hda_nid_t adc)
{
	unsigned int caps;

	caps = query_amp_caps(codec, adc, HDA_INPUT);
	caps &= ~(AC_AMPCAP_OFFSET);
	caps |= 0x02;
	snd_hda_override_amp_caps(codec, adc, HDA_INPUT, caps);
}

static int patch_cs4208(struct hda_codec *codec)
{
	struct cs_spec *spec;
	int err;

	spec = cs_alloc_spec(codec, CS4208_VENDOR_NID);
	if (!spec)
		return -ENOMEM;

	codec->patch_ops = cs_patch_ops;
	spec->gen.automute_hook = cs_automute;
	/* exclude NID 0x10 (HP) from output volumes due to different steps */
	spec->gen.out_vol_mask = 1ULL << 0x10;

	snd_hda_pick_fixup(codec, cs4208_models, cs4208_fixup_tbl,
			   cs4208_fixups);
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PRE_PROBE);

	snd_hda_override_wcaps(codec, 0x18,
			       get_wcaps(codec, 0x18) | AC_WCAP_STEREO);
	cs4208_fix_amp_caps(codec, 0x18);
	cs4208_fix_amp_caps(codec, 0x1b);
	cs4208_fix_amp_caps(codec, 0x1c);

	err = cs_parse_auto_config(codec);
	if (err < 0)
		goto error;

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PROBE);

	return 0;

 error:
	cs_free(codec);
	return err;
}

/*
 * Cirrus Logic CS4210
 *
 * 1 DAC => HP(sense) / Speakers,
 * 1 ADC <= LineIn(sense) / MicIn / DMicIn,
 * 1 SPDIF OUT => SPDIF Trasmitter(sense)
 */

/* CS4210 board names */
static const struct hda_model_fixup cs421x_models[] = {
	{ .id = CS421X_CDB4210, .name = "cdb4210" },
	{ .id = CS421X_STUMPY, .name = "stumpy" },
	{}
};

static const struct snd_pci_quirk cs421x_fixup_tbl[] = {
	/* Test Intel board + CDB2410  */
	SND_PCI_QUIRK(0x8086, 0x5001, "DP45SG/CDB4210", CS421X_CDB4210),
	{} /* terminator */
};

/* CS4210 board pinconfigs */
/* Default CS4210 (CDB4210)*/
static const struct hda_pintbl cdb4210_pincfgs[] = {
	{ 0x05, 0x0321401f },
	{ 0x06, 0x90170010 },
	{ 0x07, 0x03813031 },
	{ 0x08, 0xb7a70037 },
	{ 0x09, 0xb7a6003e },
	{ 0x0a, 0x034510f0 },
	{} /* terminator */
};

/* Stumpy ChromeBox */
static const struct hda_pintbl stumpy_pincfgs[] = {
	{ 0x05, 0x022120f0 },
	{ 0x06, 0x901700f0 },
	{ 0x07, 0x02a120f0 },
	{ 0x08, 0x77a70037 },
	{ 0x09, 0x77a6003e },
	{ 0x0a, 0x434510f0 },
	{} /* terminator */
};

/* Setup GPIO/SENSE for each board (if used) */
static void cs421x_fixup_sense_b(struct hda_codec *codec,
				 const struct hda_fixup *fix, int action)
{
	struct cs_spec *spec = codec->spec;

	if (action == HDA_FIXUP_ACT_PRE_PROBE)
		spec->sense_b = 1;
}

static const struct hda_fixup cs421x_fixups[] = {
	[CS421X_CDB4210] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = cdb4210_pincfgs,
		.chained = true,
		.chain_id = CS421X_SENSE_B,
	},
	[CS421X_SENSE_B] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs421x_fixup_sense_b,
	},
	[CS421X_STUMPY] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = stumpy_pincfgs,
	},
};

static const struct hda_verb cs421x_coef_init_verbs[] = {
	{0x0B, AC_VERB_SET_PROC_STATE, 1},
	{0x0B, AC_VERB_SET_COEF_INDEX, CS421X_IDX_DEV_CFG},
	/*
	 *  Disable Coefficient Index Auto-Increment(DAI)=1,
	 *  PDREF=0
	 */
	{0x0B, AC_VERB_SET_PROC_COEF, 0x0001 },

	{0x0B, AC_VERB_SET_COEF_INDEX, CS421X_IDX_ADC_CFG},
	/* ADC SZCMode = Digital Soft Ramp */
	{0x0B, AC_VERB_SET_PROC_COEF, 0x0002 },

	{0x0B, AC_VERB_SET_COEF_INDEX, CS421X_IDX_DAC_CFG},
	{0x0B, AC_VERB_SET_PROC_COEF,
	 (0x0002 /* DAC SZCMode = Digital Soft Ramp */
	  | 0x0004 /* Mute DAC on FIFO error */
	  | 0x0008 /* Enable DAC High Pass Filter */
	  )},
	{} /* terminator */
};

/* Errata: CS4210 rev A1 Silicon
 *
 * http://www.cirrus.com/en/pubs/errata/
 *
 * Description:
 * 1. Performance degredation is present in the ADC.
 * 2. Speaker output is not completely muted upon HP detect.
 * 3. Noise is present when clipping occurs on the amplified
 *    speaker outputs.
 *
 * Workaround:
 * The following verb sequence written to the registers during
 * initialization will correct the issues listed above.
 */

static const struct hda_verb cs421x_coef_init_verbs_A1_silicon_fixes[] = {
	{0x0B, AC_VERB_SET_PROC_STATE, 0x01},  /* VPW: processing on */

	{0x0B, AC_VERB_SET_COEF_INDEX, 0x0006},
	{0x0B, AC_VERB_SET_PROC_COEF, 0x9999}, /* Test mode: on */

	{0x0B, AC_VERB_SET_COEF_INDEX, 0x000A},
	{0x0B, AC_VERB_SET_PROC_COEF, 0x14CB}, /* Chop double */

	{0x0B, AC_VERB_SET_COEF_INDEX, 0x0011},
	{0x0B, AC_VERB_SET_PROC_COEF, 0xA2D0}, /* Increase ADC current */

	{0x0B, AC_VERB_SET_COEF_INDEX, 0x001A},
	{0x0B, AC_VERB_SET_PROC_COEF, 0x02A9}, /* Mute speaker */

	{0x0B, AC_VERB_SET_COEF_INDEX, 0x001B},
	{0x0B, AC_VERB_SET_PROC_COEF, 0X1006}, /* Remove noise */

	{} /* terminator */
};

/* Speaker Amp Gain is controlled by the vendor widget's coef 4 */
static const DECLARE_TLV_DB_SCALE(cs421x_speaker_boost_db_scale, 900, 300, 0);

static int cs421x_boost_vol_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 3;
	return 0;
}

static int cs421x_boost_vol_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] =
		cs_vendor_coef_get(codec, CS421X_IDX_SPK_CTL) & 0x0003;
	return 0;
}

static int cs421x_boost_vol_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);

	unsigned int vol = ucontrol->value.integer.value[0];
	unsigned int coef =
		cs_vendor_coef_get(codec, CS421X_IDX_SPK_CTL);
	unsigned int original_coef = coef;

	coef &= ~0x0003;
	coef |= (vol & 0x0003);
	if (original_coef != coef) {
		cs_vendor_coef_set(codec, CS421X_IDX_SPK_CTL, coef);
		return 1;
	}

	return 0;
}

static const struct snd_kcontrol_new cs421x_speaker_boost_ctl = {

	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = (SNDRV_CTL_ELEM_ACCESS_READWRITE |
			SNDRV_CTL_ELEM_ACCESS_TLV_READ),
	.name = "Speaker Boost Playback Volume",
	.info = cs421x_boost_vol_info,
	.get = cs421x_boost_vol_get,
	.put = cs421x_boost_vol_put,
	.tlv = { .p = cs421x_speaker_boost_db_scale },
};

static void cs4210_pinmux_init(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	unsigned int def_conf, coef;

	/* GPIO, DMIC_SCL, DMIC_SDA and SENSE_B are multiplexed */
	coef = cs_vendor_coef_get(codec, CS421X_IDX_DEV_CFG);

	if (spec->gpio_mask)
		coef |= 0x0008; /* B1,B2 are GPIOs */
	else
		coef &= ~0x0008;

	if (spec->sense_b)
		coef |= 0x0010; /* B2 is SENSE_B, not inverted  */
	else
		coef &= ~0x0010;

	cs_vendor_coef_set(codec, CS421X_IDX_DEV_CFG, coef);

	if ((spec->gpio_mask || spec->sense_b) &&
	    is_active_pin(codec, CS421X_DMIC_PIN_NID)) {

		/*
		 *  GPIO or SENSE_B forced - disconnect the DMIC pin.
		 */
		def_conf = snd_hda_codec_get_pincfg(codec, CS421X_DMIC_PIN_NID);
		def_conf &= ~AC_DEFCFG_PORT_CONN;
		def_conf |= (AC_JACK_PORT_NONE << AC_DEFCFG_PORT_CONN_SHIFT);
		snd_hda_codec_set_pincfg(codec, CS421X_DMIC_PIN_NID, def_conf);
	}
}

static void cs4210_spdif_automute(struct hda_codec *codec,
				  struct hda_jack_callback *tbl)
{
	struct cs_spec *spec = codec->spec;
	bool spdif_present = false;
	hda_nid_t spdif_pin = spec->gen.autocfg.dig_out_pins[0];

	/* detect on spdif is specific to CS4210 */
	if (!spec->spdif_detect ||
	    spec->vendor_nid != CS4210_VENDOR_NID)
		return;

	spdif_present = snd_hda_jack_detect(codec, spdif_pin);
	if (spdif_present == spec->spdif_present)
		return;

	spec->spdif_present = spdif_present;
	/* SPDIF TX on/off */
	snd_hda_set_pin_ctl(codec, spdif_pin, spdif_present ? PIN_OUT : 0);

	cs_automute(codec);
}

static void parse_cs421x_digital(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	struct auto_pin_cfg *cfg = &spec->gen.autocfg;
	int i;

	for (i = 0; i < cfg->dig_outs; i++) {
		hda_nid_t nid = cfg->dig_out_pins[i];

		if (get_wcaps(codec, nid) & AC_WCAP_UNSOL_CAP) {
			spec->spdif_detect = 1;
			snd_hda_jack_detect_enable_callback(codec, nid,
							    cs4210_spdif_automute);
		}
	}
}

static int cs421x_init(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	if (spec->vendor_nid == CS4210_VENDOR_NID) {
		snd_hda_sequence_write(codec, cs421x_coef_init_verbs);
		snd_hda_sequence_write(codec, cs421x_coef_init_verbs_A1_silicon_fixes);
		cs4210_pinmux_init(codec);
	}

	snd_hda_gen_init(codec);

	if (spec->gpio_mask) {
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_MASK,
				    spec->gpio_mask);
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DIRECTION,
				    spec->gpio_dir);
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DATA,
				    spec->gpio_data);
	}

	init_input_coef(codec);

	cs4210_spdif_automute(codec, NULL);

	return 0;
}

static void fix_volume_caps(struct hda_codec *codec, hda_nid_t dac)
{
	unsigned int caps;

	/* set the upper-limit for mixer amp to 0dB */
	caps = query_amp_caps(codec, dac, HDA_OUTPUT);
	caps &= ~(0x7f << AC_AMPCAP_NUM_STEPS_SHIFT);
	caps |= ((caps >> AC_AMPCAP_OFFSET_SHIFT) & 0x7f)
		<< AC_AMPCAP_NUM_STEPS_SHIFT;
	snd_hda_override_amp_caps(codec, dac, HDA_OUTPUT, caps);
}

static int cs421x_parse_auto_config(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	hda_nid_t dac = CS4210_DAC_NID;
	int err;

	fix_volume_caps(codec, dac);

	err = snd_hda_parse_pin_defcfg(codec, &spec->gen.autocfg, NULL, 0);
	if (err < 0)
		return err;

	err = snd_hda_gen_parse_auto_config(codec, &spec->gen.autocfg);
	if (err < 0)
		return err;

	parse_cs421x_digital(codec);

	if (spec->gen.autocfg.speaker_outs &&
	    spec->vendor_nid == CS4210_VENDOR_NID) {
		if (!snd_hda_gen_add_kctl(&spec->gen, NULL,
					  &cs421x_speaker_boost_ctl))
			return -ENOMEM;
	}

	return 0;
}

#ifdef CONFIG_PM
/*
 *	Manage PDREF, when transitioning to D3hot
 *	(DAC,ADC) -> D3, PDREF=1, AFG->D3
 */
static int cs421x_suspend(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	unsigned int coef;

	snd_hda_shutup_pins(codec);

	snd_hda_codec_write(codec, CS4210_DAC_NID, 0,
			    AC_VERB_SET_POWER_STATE,  AC_PWRST_D3);
	snd_hda_codec_write(codec, CS4210_ADC_NID, 0,
			    AC_VERB_SET_POWER_STATE,  AC_PWRST_D3);

	if (spec->vendor_nid == CS4210_VENDOR_NID) {
		coef = cs_vendor_coef_get(codec, CS421X_IDX_DEV_CFG);
		coef |= 0x0004; /* PDREF */
		cs_vendor_coef_set(codec, CS421X_IDX_DEV_CFG, coef);
	}

	return 0;
}
#endif

static const struct hda_codec_ops cs421x_patch_ops = {
	.build_controls = snd_hda_gen_build_controls,
	.build_pcms = snd_hda_gen_build_pcms,
	.init = cs421x_init,
	.free = cs_free,
	.unsol_event = snd_hda_jack_unsol_event,
#ifdef CONFIG_PM
	.suspend = cs421x_suspend,
#endif
};

static int patch_cs4210(struct hda_codec *codec)
{
	struct cs_spec *spec;
	int err;

	spec = cs_alloc_spec(codec, CS4210_VENDOR_NID);
	if (!spec)
		return -ENOMEM;

	codec->patch_ops = cs421x_patch_ops;
	spec->gen.automute_hook = cs_automute;

	snd_hda_pick_fixup(codec, cs421x_models, cs421x_fixup_tbl,
			   cs421x_fixups);
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PRE_PROBE);

	/*
	 *  Update the GPIO/DMIC/SENSE_B pinmux before the configuration
	 *   is auto-parsed. If GPIO or SENSE_B is forced, DMIC input
	 *   is disabled.
	 */
	cs4210_pinmux_init(codec);

	err = cs421x_parse_auto_config(codec);
	if (err < 0)
		goto error;

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PROBE);

	return 0;

 error:
	cs_free(codec);
	return err;
}

static int patch_cs4213(struct hda_codec *codec)
{
	struct cs_spec *spec;
	int err;

	spec = cs_alloc_spec(codec, CS4213_VENDOR_NID);
	if (!spec)
		return -ENOMEM;

	codec->patch_ops = cs421x_patch_ops;

	err = cs421x_parse_auto_config(codec);
	if (err < 0)
		goto error;

	return 0;

 error:
	cs_free(codec);
	return err;
}

/* Cirrus Logic CS8409 HDA bridge with
 * companion codec CS42L42
 */
#define CS8409_VENDOR_NID 0x47

#define CS8409_CS42L42_HP_PIN_NID	0x24
#define CS8409_CS42L42_SPK_PIN_NID	0x2c
#define CS8409_CS42L42_AMIC_PIN_NID	0x34
#define CS8409_CS42L42_DMIC_PIN_NID	0x44
#define CS8409_CS42L42_DMIC_ADC_PIN_NID	0x22

#define CS42L42_HSDET_AUTO_DONE	0x02
#define CS42L42_HSTYPE_MASK		0x03

#define CS42L42_JACK_INSERTED	0x0C
#define CS42L42_JACK_REMOVED	0x00

#define GPIO3_INT (1 << 3)
#define GPIO4_INT (1 << 4)
#define GPIO5_INT (1 << 5)

#define CS42L42_I2C_ADDR	(0x48 << 1)

#define CIR_I2C_ADDR	0x0059
#define CIR_I2C_DATA	0x005A
#define CIR_I2C_CTRL	0x005B
#define CIR_I2C_STATUS	0x005C
#define CIR_I2C_QWRITE	0x005D
#define CIR_I2C_QREAD	0x005E

#define CS8409_CS42L42_HP_VOL_REAL_MIN   (-63)
#define CS8409_CS42L42_HP_VOL_REAL_MAX   (0)
#define CS8409_CS42L42_AMIC_VOL_REAL_MIN (-97)
#define CS8409_CS42L42_AMIC_VOL_REAL_MAX (12)
#define CS8409_CS42L42_REG_HS_VOLUME_CHA (0x2301)
#define CS8409_CS42L42_REG_HS_VOLUME_CHB (0x2303)
#define CS8409_CS42L42_REG_AMIC_VOLUME   (0x1D03)

struct cs8409_i2c_param {
	unsigned int addr;
	unsigned int reg;
};

struct cs8409_cir_param {
	unsigned int nid;
	unsigned int cir;
	unsigned int coeff;
};

enum {
	CS8409_BULLSEYE,
	CS8409_WARLOCK,
	CS8409_CYBORG,
	CS8409_FIXUPS,
	CS8409_MBP131,
	CS8409_GPIO_0,
	CS8409_MBP143,
	CS8409_GPIO,
};

static void cs8409_cs42l42_fixups(struct hda_codec *codec,
				    const struct hda_fixup *fix, int action);
static int cs8409_cs42l42_exec_verb(struct hdac_device *dev,
		unsigned int cmd, unsigned int flags, unsigned int *res);

static void cs_8409_fixup_gpio(struct hda_codec *codec,
                               const struct hda_fixup *fix, int action);

/* Dell Inspiron models with cs8409/cs42l42 */
static const struct hda_model_fixup cs8409_models[] = {
	{ .id = CS8409_BULLSEYE, .name = "bullseye" },
	{ .id = CS8409_WARLOCK, .name = "warlock" },
	{ .id = CS8409_CYBORG, .name = "cyborg" },
	{ .id = CS8409_MBP131, .name = "mbp131" },
	{ .id = CS8409_MBP143, .name = "mbp143" },
};

/* Dell Inspiron platforms
 * with cs8409 bridge and cs42l42 codec
 */
static const struct snd_pci_quirk cs8409_fixup_tbl[] = {
	SND_PCI_QUIRK(0x1028, 0x0A11, "Bullseye", CS8409_BULLSEYE),
	SND_PCI_QUIRK(0x1028, 0x0A12, "Bullseye", CS8409_BULLSEYE),
	SND_PCI_QUIRK(0x1028, 0x0A23, "Bullseye", CS8409_BULLSEYE),
	SND_PCI_QUIRK(0x1028, 0x0A24, "Bullseye", CS8409_BULLSEYE),
	SND_PCI_QUIRK(0x1028, 0x0A25, "Bullseye", CS8409_BULLSEYE),
	SND_PCI_QUIRK(0x1028, 0x0A29, "Bullseye", CS8409_BULLSEYE),
	SND_PCI_QUIRK(0x1028, 0x0A2A, "Bullseye", CS8409_BULLSEYE),
	SND_PCI_QUIRK(0x1028, 0x0A2B, "Bullseye", CS8409_BULLSEYE),
	SND_PCI_QUIRK(0x1028, 0x0AB0, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0AB2, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0AB1, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0AB3, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0AB4, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0AB5, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0AD9, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0ADA, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0ADB, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0ADC, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0AF4, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0AF5, "Warlock", CS8409_WARLOCK),
	SND_PCI_QUIRK(0x1028, 0x0A77, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0A78, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0A79, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0A7A, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0A7D, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0A7E, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0A7F, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0A80, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0ADF, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AE0, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AE1, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AE2, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AE9, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AEA, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AEB, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AEC, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AED, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AEE, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AEF, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x1028, 0x0AF0, "Cyborg", CS8409_CYBORG),
	SND_PCI_QUIRK(0x106b, 0x3300, "MacBookPro 13,1", CS8409_MBP131),
	SND_PCI_QUIRK(0x106b, 0x3900, "MacBookPro 14,3", CS8409_MBP143),
	{} /* terminator */
};

static const struct hda_verb cs8409_cs42l42_init_verbs[] = {
	{ 0x01, AC_VERB_SET_GPIO_WAKE_MASK, 0x0018 }, /* WAKE from GPIO 3,4 */
	{ 0x47, AC_VERB_SET_PROC_STATE, 0x0001 },     /* Enable VPW processing  */
	{ 0x47, AC_VERB_SET_COEF_INDEX, 0x0002 },     /* Configure GPIO 6,7 */
	{ 0x47, AC_VERB_SET_PROC_COEF,  0x0080 },     /* I2C mode */
	{ 0x47, AC_VERB_SET_COEF_INDEX, 0x005b },     /* Set I2C bus speed */
	{ 0x47, AC_VERB_SET_PROC_COEF,  0x0200 },     /* 100kHz I2C_STO = 2 */
	{} /* terminator */
};

static const struct hda_pintbl cs8409_cs42l42_pincfgs[] = {
	{ 0x24, 0x042120f0 }, /* ASP-1-TX */
	{ 0x34, 0x04a12050 }, /* ASP-1-RX */
	{ 0x2c, 0x901000f0 }, /* ASP-2-TX */
	{ 0x44, 0x90a00090 }, /* DMIC-1 */
	{} /* terminator */
};

static const struct hda_pintbl mbp131_pincfgs[] = {
       {} /* terminator */
};

static const struct hda_pintbl mbp143_pincfgs[] = {
       {} /* terminator */
};

static const struct hda_fixup cs8409_fixups[] = {
	[CS8409_BULLSEYE] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = cs8409_cs42l42_pincfgs,
		.chained = true,
		.chain_id = CS8409_FIXUPS,
	},
	[CS8409_WARLOCK] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = cs8409_cs42l42_pincfgs,
		.chained = true,
		.chain_id = CS8409_FIXUPS,
	},
	[CS8409_CYBORG] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = cs8409_cs42l42_pincfgs,
		.chained = true,
		.chain_id = CS8409_FIXUPS,
	},
	[CS8409_FIXUPS] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs8409_cs42l42_fixups,
	},
	[CS8409_MBP131] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = mbp131_pincfgs,
		.chained = true,
		.chain_id = CS8409_GPIO_0,
	},
	[CS8409_GPIO_0] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs_8409_fixup_gpio,
	},
	[CS8409_MBP143] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = mbp143_pincfgs,
		.chained = true,
		.chain_id = CS8409_GPIO,
	},
	[CS8409_GPIO] = {
		.type = HDA_FIXUP_FUNC,
		.v.func = cs_8409_fixup_gpio,
	},
};

/* Vendor specific HW configuration for CS42L42 */
static const struct cs8409_i2c_param cs42l42_init_reg_seq[] = {
	{ 0x1010, 0xB0 },
	{ 0x1D01, 0x00 },
	{ 0x1D02, 0x06 },
	{ 0x1D03, 0x00 },
	{ 0x1107, 0x01 },
	{ 0x1009, 0x02 },
	{ 0x1007, 0x03 },
	{ 0x1201, 0x00 },
	{ 0x1208, 0x13 },
	{ 0x1205, 0xFF },
	{ 0x1206, 0x00 },
	{ 0x1207, 0x20 },
	{ 0x1202, 0x0D },
	{ 0x2A02, 0x02 },
	{ 0x2A03, 0x00 },
	{ 0x2A04, 0x00 },
	{ 0x2A05, 0x02 },
	{ 0x2A06, 0x00 },
	{ 0x2A07, 0x20 },
	{ 0x2A08, 0x02 },
	{ 0x2A09, 0x00 },
	{ 0x2A0A, 0x80 },
	{ 0x2A0B, 0x02 },
	{ 0x2A0C, 0x00 },
	{ 0x2A0D, 0xA0 },
	{ 0x2A01, 0x0C },
	{ 0x2902, 0x01 },
	{ 0x2903, 0x02 },
	{ 0x2904, 0x00 },
	{ 0x2905, 0x00 },
	{ 0x2901, 0x01 },
	{ 0x1101, 0x0A },
	{ 0x1102, 0x84 },
	{ 0x2301, 0x00 },
	{ 0x2303, 0x00 },
	{ 0x2302, 0x3f },
	{ 0x2001, 0x03 },
	{ 0x1B75, 0xB6 },
	{ 0x1B73, 0xC2 },
	{ 0x1129, 0x01 },
	{ 0x1121, 0xF3 },
	{ 0x1103, 0x20 },
	{ 0x1105, 0x00 },
	{ 0x1112, 0xC0 },
	{ 0x1113, 0x80 },
	{ 0x1C03, 0xC0 },
	{ 0x1105, 0x00 },
	{ 0x1112, 0xC0 },
	{ 0x1101, 0x02 },
	{} /* Terminator */
};

/* Vendor specific hw configuration for CS8409 */
static const struct cs8409_cir_param cs8409_cs42l42_hw_cfg[] = {
	{ 0x47, 0x00, 0xb008 }, /* +PLL1/2_EN, +I2C_EN */
	{ 0x47, 0x01, 0x0002 }, /* ASP1/2_EN=0, ASP1_STP=1 */
	{ 0x47, 0x02, 0x0a80 }, /* ASP1/2_BUS_IDLE=10, +GPIO_I2C */
	{ 0x47, 0x19, 0x0800 }, /* ASP1.A: TX.LAP=0, TX.LSZ=24 bits, TX.LCS=0 */
	{ 0x47, 0x1a, 0x0820 }, /* ASP1.A: TX.RAP=0, TX.RSZ=24 bits, TX.RCS=32 */
	{ 0x47, 0x29, 0x0800 }, /* ASP2.A: TX.LAP=0, TX.LSZ=24 bits, TX.LCS=0 */
	{ 0x47, 0x2a, 0x2800 }, /* ASP2.A: TX.RAP=1, TX.RSZ=24 bits, TX.RCS=0 */
	{ 0x47, 0x39, 0x0800 }, /* ASP1.A: RX.LAP=0, RX.LSZ=24 bits, RX.LCS=0 */
	{ 0x47, 0x3a, 0x0800 }, /* ASP1.A: RX.RAP=0, RX.RSZ=24 bits, RX.RCS=0 */
	{ 0x47, 0x03, 0x8000 }, /* ASP1: LCHI = 00h */
	{ 0x47, 0x04, 0x28ff }, /* ASP1: MC/SC_SRCSEL=PLL1, LCPR=FFh */
	{ 0x47, 0x05, 0x0062 }, /* ASP1: MCEN=0, FSD=011, SCPOL_IN/OUT=0, SCDIV=1:4 */
	{ 0x47, 0x06, 0x801f }, /* ASP2: LCHI=1Fh */
	{ 0x47, 0x07, 0x283f }, /* ASP2: MC/SC_SRCSEL=PLL1, LCPR=3Fh */
	{ 0x47, 0x08, 0x805c }, /* ASP2: 5050=1, MCEN=0, FSD=010, SCPOL_IN/OUT=1, SCDIV=1:16 */
	{ 0x47, 0x09, 0x0023 }, /* DMIC1_MO=10b, DMIC1/2_SR=1 */
	{ 0x47, 0x0a, 0x0000 }, /* ASP1/2_BEEP=0 */
	{ 0x47, 0x01, 0x0062 }, /* ASP1/2_EN=1, ASP1_STP=1 */
	{ 0x47, 0x00, 0x9008 }, /* -PLL2_EN */
	{ 0x47, 0x68, 0x0000 }, /* TX2.A: pre-scale att.=0 dB */
	{ 0x47, 0x82, 0xfc03 }, /* ASP1/2_xxx_EN=1, ASP1/2_MCLK_EN=0, DMIC1_SCL_EN=1 */
	{ 0x47, 0xc0, 0x9999 }, /* test mode on */
	{ 0x47, 0xc5, 0x0000 }, /* GPIO hysteresis = 30 us */
	{ 0x47, 0xc0, 0x0000 }, /* test mode off */
	{} /* Terminator */
};

static const struct cs8409_cir_param cs8409_cs42l42_bullseye_atn[] = {
	{ 0x47, 0x65, 0x4000 }, /* EQ_SEL=1, EQ1/2_EN=0 */
	{ 0x47, 0x64, 0x4000 }, /* +EQ_ACC */
	{ 0x47, 0x65, 0x4010 }, /* +EQ2_EN */
	{ 0x47, 0x63, 0x0647 }, /* EQ_DATA_HI=0x0647 */
	{ 0x47, 0x64, 0xc0c7 }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=0, EQ_DATA_LO=0x67 */
	{ 0x47, 0x63, 0x0647 }, /* EQ_DATA_HI=0x0647 */
	{ 0x47, 0x64, 0xc1c7 }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=1, EQ_DATA_LO=0x67 */
	{ 0x47, 0x63, 0xf370 }, /* EQ_DATA_HI=0xf370 */
	{ 0x47, 0x64, 0xc271 }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=2, EQ_DATA_LO=0x71 */
	{ 0x47, 0x63, 0x1ef8 }, /* EQ_DATA_HI=0x1ef8 */
	{ 0x47, 0x64, 0xc348 }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=3, EQ_DATA_LO=0x48 */
	{ 0x47, 0x63, 0xc110 }, /* EQ_DATA_HI=0xc110 */
	{ 0x47, 0x64, 0xc45a }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=4, EQ_DATA_LO=0x5a */
	{ 0x47, 0x63, 0x1f29 }, /* EQ_DATA_HI=0x1f29 */
	{ 0x47, 0x64, 0xc574 }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=5, EQ_DATA_LO=0x74 */
	{ 0x47, 0x63, 0x1d7a }, /* EQ_DATA_HI=0x1d7a */
	{ 0x47, 0x64, 0xc653 }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=6, EQ_DATA_LO=0x53 */
	{ 0x47, 0x63, 0xc38c }, /* EQ_DATA_HI=0xc38c */
	{ 0x47, 0x64, 0xc714 }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=7, EQ_DATA_LO=0x14 */
	{ 0x47, 0x63, 0x1ca3 }, /* EQ_DATA_HI=0x1ca3 */
	{ 0x47, 0x64, 0xc8c7 }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=8, EQ_DATA_LO=0xc7 */
	{ 0x47, 0x63, 0xc38c }, /* EQ_DATA_HI=0xc38c */
	{ 0x47, 0x64, 0xc914 }, /* +EQ_WRT, +EQ_ACC, EQ_ADR=9, EQ_DATA_LO=0x14 */
	{ 0x47, 0x64, 0x0000 }, /* -EQ_ACC, -EQ_WRT */
	{} /* Terminator */
};

/**
 * cs8409_enable_i2c_clock - Enable I2C clocks
 * @codec: the codec instance
 * @enable: Enable or disable I2C clocks
 *
 * Enable or Disable I2C clocks.
 */
static void cs8409_enable_i2c_clock(struct hda_codec *codec, unsigned int enable)
{
	unsigned int retval;
	unsigned int newval;

	retval = cs_vendor_coef_get(codec, 0x0);
	newval = (enable) ? (retval | 0x8) : (retval & 0xfffffff7);
	cs_vendor_coef_set(codec, 0x0, newval);
}

/**
 * cs8409_i2c_wait_complete - Wait for I2C transaction
 * @codec: the codec instance
 *
 * Wait for I2C transaction to complete.
 * Return -1 if transaction wait times out.
 */
static int cs8409_i2c_wait_complete(struct hda_codec *codec)
{
	int repeat = 5;
	unsigned int retval;

	do {
		retval = cs_vendor_coef_get(codec, CIR_I2C_STATUS);
		if ((retval & 0x18) != 0x18) {
			usleep_range(2000, 4000);
			--repeat;
		} else
			return 0;

	} while (repeat);

	return -1;
}

/**
 * cs8409_i2c_read - CS8409 I2C Read.
 * @codec: the codec instance
 * @i2c_address: I2C Address
 * @i2c_reg: Register to read
 * @paged: Is a paged transaction
 *
 * CS8409 I2C Read.
 * Returns negative on error, otherwise returns read value in bits 0-7.
 */
static int cs8409_i2c_read(struct hda_codec *codec,
		unsigned int i2c_address,
		unsigned int i2c_reg,
		unsigned int paged)
{
	unsigned int i2c_reg_data;
	unsigned int read_data;

	cs8409_enable_i2c_clock(codec, 1);
	cs_vendor_coef_set(codec, CIR_I2C_ADDR, i2c_address);

	if (paged) {
		cs_vendor_coef_set(codec, CIR_I2C_QWRITE, i2c_reg >> 8);
		if (cs8409_i2c_wait_complete(codec) < 0) {
			codec_err(codec,
				"%s() Paged Transaction Failed 0x%02x : 0x%04x\n",
				__func__, i2c_address, i2c_reg);
			return -EIO;
		}
	}

	i2c_reg_data = (i2c_reg << 8) & 0x0ffff;
	cs_vendor_coef_set(codec, CIR_I2C_QREAD, i2c_reg_data);
	if (cs8409_i2c_wait_complete(codec) < 0) {
		codec_err(codec, "%s() Transaction Failed 0x%02x : 0x%04x\n",
			__func__, i2c_address, i2c_reg);
		return -EIO;
	}

	/* Register in bits 15-8 and the data in 7-0 */
	read_data = cs_vendor_coef_get(codec, CIR_I2C_QREAD);

	cs8409_enable_i2c_clock(codec, 0);

	return read_data & 0x0ff;
}

/**
 * cs8409_i2c_write - CS8409 I2C Write.
 * @codec: the codec instance
 * @i2c_address: I2C Address
 * @i2c_reg: Register to write to
 * @i2c_data: Data to write
 * @paged: Is a paged transaction
 *
 * CS8409 I2C Write.
 * Returns negative on error, otherwise returns 0.
 */
static int cs8409_i2c_write(struct hda_codec *codec,
		unsigned int i2c_address, unsigned int i2c_reg,
		unsigned int i2c_data,
		unsigned int paged)
{
	unsigned int i2c_reg_data;

	cs8409_enable_i2c_clock(codec, 1);
	cs_vendor_coef_set(codec, CIR_I2C_ADDR, i2c_address);

	if (paged) {
		cs_vendor_coef_set(codec, CIR_I2C_QWRITE, i2c_reg >> 8);
		if (cs8409_i2c_wait_complete(codec) < 0) {
			codec_err(codec,
				"%s() Paged Transaction Failed 0x%02x : 0x%04x\n",
				__func__, i2c_address, i2c_reg);
			return -EIO;
		}
	}

	i2c_reg_data = ((i2c_reg << 8) & 0x0ff00) | (i2c_data & 0x0ff);
	cs_vendor_coef_set(codec, CIR_I2C_QWRITE, i2c_reg_data);

	if (cs8409_i2c_wait_complete(codec) < 0) {
		codec_err(codec, "%s() Transaction Failed 0x%02x : 0x%04x\n",
			__func__, i2c_address, i2c_reg);
		return -EIO;
	}

	cs8409_enable_i2c_clock(codec, 0);

	return 0;
}

static int cs8409_cs42l42_volume_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *uinfo)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	u16 nid = get_amp_nid(kcontrol);
	u8 chs = get_amp_channels(kcontrol);

	codec_dbg(codec, "%s() nid: %d\n", __func__, nid);
	switch (nid) {
	case CS8409_CS42L42_HP_PIN_NID:
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		uinfo->count = chs == 3 ? 2 : 1;
		uinfo->value.integer.min = CS8409_CS42L42_HP_VOL_REAL_MIN;
		uinfo->value.integer.max = CS8409_CS42L42_HP_VOL_REAL_MAX;
		break;
	case CS8409_CS42L42_AMIC_PIN_NID:
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		uinfo->count = chs == 3 ? 2 : 1;
		uinfo->value.integer.min = CS8409_CS42L42_AMIC_VOL_REAL_MIN;
		uinfo->value.integer.max = CS8409_CS42L42_AMIC_VOL_REAL_MAX;
		break;
	default:
		break;
	}
	return 0;
}

static void cs8409_cs42l42_update_volume(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	int data;

	mutex_lock(&spec->cs8409_i2c_mux);
	data = cs8409_i2c_read(codec, CS42L42_I2C_ADDR,
				CS8409_CS42L42_REG_HS_VOLUME_CHA, 1);
	if (data >= 0)
		spec->cs42l42_hp_volume[0] = -data;
	else
		spec->cs42l42_hp_volume[0] = CS8409_CS42L42_HP_VOL_REAL_MIN;
	data = cs8409_i2c_read(codec, CS42L42_I2C_ADDR,
				CS8409_CS42L42_REG_HS_VOLUME_CHB, 1);
	if (data >= 0)
		spec->cs42l42_hp_volume[1] = -data;
	else
		spec->cs42l42_hp_volume[1] = CS8409_CS42L42_HP_VOL_REAL_MIN;
	data = cs8409_i2c_read(codec, CS42L42_I2C_ADDR,
				CS8409_CS42L42_REG_AMIC_VOLUME, 1);
	if (data >= 0)
		spec->cs42l42_hs_mic_volume[0] = -data;
	else
		spec->cs42l42_hs_mic_volume[0] = CS8409_CS42L42_AMIC_VOL_REAL_MIN;
	mutex_unlock(&spec->cs8409_i2c_mux);
	spec->cs42l42_volume_init = 1;
}

static int cs8409_cs42l42_volume_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cs_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	int chs = get_amp_channels(kcontrol);
	long *valp = ucontrol->value.integer.value;

	if (!spec->cs42l42_volume_init) {
		snd_hda_power_up(codec);
		cs8409_cs42l42_update_volume(codec);
		snd_hda_power_down(codec);
	}
	switch (nid) {
	case CS8409_CS42L42_HP_PIN_NID:
		if (chs & BIT(0))
			*valp++ = spec->cs42l42_hp_volume[0];
		if (chs & BIT(1))
			*valp++ = spec->cs42l42_hp_volume[1];
		break;
	case CS8409_CS42L42_AMIC_PIN_NID:
		if (chs & BIT(0))
			*valp++ = spec->cs42l42_hs_mic_volume[0];
		break;
	default:
		break;
	}
	return 0;
}

static int cs8409_cs42l42_volume_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cs_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	int chs = get_amp_channels(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int change = 0;
	char vol;

	snd_hda_power_up(codec);
	switch (nid) {
	case CS8409_CS42L42_HP_PIN_NID:
		mutex_lock(&spec->cs8409_i2c_mux);
		if (chs & BIT(0)) {
			vol = -(*valp);
			change = cs8409_i2c_write(codec, CS42L42_I2C_ADDR,
				CS8409_CS42L42_REG_HS_VOLUME_CHA, vol, 1);
			valp++;
		}
		if (chs & BIT(1)) {
			vol = -(*valp);
			change |= cs8409_i2c_write(codec, CS42L42_I2C_ADDR,
				CS8409_CS42L42_REG_HS_VOLUME_CHB, vol, 1);
		}
		mutex_unlock(&spec->cs8409_i2c_mux);
		break;
	case CS8409_CS42L42_AMIC_PIN_NID:
		mutex_lock(&spec->cs8409_i2c_mux);
		if (chs & BIT(0)) {
			change = cs8409_i2c_write(
				codec, CS42L42_I2C_ADDR,
				CS8409_CS42L42_REG_AMIC_VOLUME, (char)*valp, 1);
			valp++;
		}
		mutex_unlock(&spec->cs8409_i2c_mux);
		break;
	default:
		break;
	}
	cs8409_cs42l42_update_volume(codec);
	snd_hda_power_down(codec);
	return change;
}

static const DECLARE_TLV_DB_SCALE(
	cs8409_cs42l42_hp_db_scale,
	CS8409_CS42L42_HP_VOL_REAL_MIN * 100, 100, 1);

static const DECLARE_TLV_DB_SCALE(
	cs8409_cs42l42_amic_db_scale,
	CS8409_CS42L42_AMIC_VOL_REAL_MIN * 100, 100, 1);

static const struct snd_kcontrol_new cs8409_cs42l42_hp_volume_mixer = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.index = 0,
	.name = "Headphone Playback Volume",
	.subdevice = (HDA_SUBDEV_AMP_FLAG | HDA_SUBDEV_NID_FLAG),
	.access = (SNDRV_CTL_ELEM_ACCESS_READWRITE
			 | SNDRV_CTL_ELEM_ACCESS_TLV_READ),
	.info = cs8409_cs42l42_volume_info,
	.get = cs8409_cs42l42_volume_get,
	.put = cs8409_cs42l42_volume_put,
	.tlv = { .p = cs8409_cs42l42_hp_db_scale },
	.private_value = HDA_COMPOSE_AMP_VAL(
		CS8409_CS42L42_HP_PIN_NID, 3, 0, HDA_OUTPUT)
		| HDA_AMP_VAL_MIN_MUTE
};

static const struct snd_kcontrol_new cs8409_cs42l42_amic_volume_mixer = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.index = 0,
	.name = "Mic Capture Volume",
	.subdevice = (HDA_SUBDEV_AMP_FLAG | HDA_SUBDEV_NID_FLAG),
	.access = (SNDRV_CTL_ELEM_ACCESS_READWRITE
			 | SNDRV_CTL_ELEM_ACCESS_TLV_READ),
	.info = cs8409_cs42l42_volume_info,
	.get = cs8409_cs42l42_volume_get,
	.put = cs8409_cs42l42_volume_put,
	.tlv = { .p = cs8409_cs42l42_amic_db_scale },
	.private_value = HDA_COMPOSE_AMP_VAL(
		CS8409_CS42L42_AMIC_PIN_NID, 1, 0, HDA_INPUT)
		| HDA_AMP_VAL_MIN_MUTE
};

/* Assert/release RTS# line to CS42L42 */
static void cs8409_cs42l42_reset(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	/* Assert RTS# line */
	snd_hda_codec_write(codec,
			codec->core.afg, 0, AC_VERB_SET_GPIO_DATA, 0);
	/* wait ~10ms */
	usleep_range(10000, 15000);
	/* Release RTS# line */
	snd_hda_codec_write(codec,
			codec->core.afg, 0, AC_VERB_SET_GPIO_DATA, GPIO5_INT);
	/* wait ~10ms */
	usleep_range(10000, 15000);

	mutex_lock(&spec->cs8409_i2c_mux);

	/* Clear interrupts, by reading interrupt status registers */
	cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x1308, 1);
	cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x1309, 1);
	cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x130A, 1);
	cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x130F, 1);

	mutex_unlock(&spec->cs8409_i2c_mux);

}

/* Configure CS42L42 slave codec for jack autodetect */
static void cs8409_cs42l42_enable_jack_detect(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	mutex_lock(&spec->cs8409_i2c_mux);

	/* Set TIP_SENSE_EN for analog front-end of tip sense. */
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1b70, 0x0020, 1);
	/* Clear WAKE# */
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1b71, 0x0001, 1);
	/* Wait ~2.5ms */
	usleep_range(2500, 3000);
	/* Set mode WAKE# output follows the combination logic directly */
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1b71, 0x0020, 1);
	/* Clear interrupts status */
	cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x130f, 1);
	cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x1b7b, 1);
	/* Enable interrupt */
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1320, 0x03, 1);
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1b79, 0x00, 1);

	mutex_unlock(&spec->cs8409_i2c_mux);
}

/* Enable and run CS42L42 slave codec jack auto detect */
static void cs8409_cs42l42_run_jack_detect(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	mutex_lock(&spec->cs8409_i2c_mux);

	/* Clear interrupts */
	cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x1308, 1);
	cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x1b77, 1);

	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1102, 0x87, 1);
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1f06, 0x86, 1);
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1b74, 0x07, 1);
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x131b, 0x01, 1);
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1120, 0x80, 1);
	/* Wait ~110ms*/
	usleep_range(110000, 200000);
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x111f, 0x77, 1);
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1120, 0xc0, 1);
	/* Wait ~10ms */
	usleep_range(10000, 25000);

	mutex_unlock(&spec->cs8409_i2c_mux);

}

static void cs8409_cs42l42_reg_setup(struct hda_codec *codec)
{
	const struct cs8409_i2c_param *seq = cs42l42_init_reg_seq;
	struct cs_spec *spec = codec->spec;

	mutex_lock(&spec->cs8409_i2c_mux);

	for (; seq->addr; seq++)
		cs8409_i2c_write(codec, CS42L42_I2C_ADDR, seq->addr, seq->reg, 1);

	mutex_unlock(&spec->cs8409_i2c_mux);

}

/*
 * In the case of CS8409 we do not have unsolicited events from NID's 0x24
 * and 0x34 where hs mic and hp are connected. Companion codec CS42L42 will
 * generate interrupt via gpio 4 to notify jack events. We have to overwrite
 * generic snd_hda_jack_unsol_event(), read CS42L42 jack detect status registers
 * and then notify status via generic snd_hda_jack_unsol_event() call.
 */
static void cs8409_jack_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct cs_spec *spec = codec->spec;
	int status_changed = 0;
	int reg_cdc_status;
	int reg_hs_status;
	int reg_ts_status;
	int type;
	struct hda_jack_tbl *jk;

	/* jack_unsol_event() will be called every time gpio line changing state.
	 * In this case gpio4 line goes up as a result of reading interrupt status
	 * registers in previous cs8409_jack_unsol_event() call.
	 * We don't need to handle this event, ignoring...
	 */
	if ((res & (1 << 4)))
		return;

	mutex_lock(&spec->cs8409_i2c_mux);

	/* Read jack detect status registers */
	reg_cdc_status = cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x1308, 1);
	reg_hs_status = cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x1124, 1);
	reg_ts_status = cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x130f, 1);

	/* Clear interrupts, by reading interrupt status registers */
	cs8409_i2c_read(codec, CS42L42_I2C_ADDR, 0x1b7b, 1);

	mutex_unlock(&spec->cs8409_i2c_mux);

	/* If status values are < 0, read error has occurred. */
	if (reg_cdc_status < 0 || reg_hs_status < 0 || reg_ts_status < 0)
		return;

	/* HSDET_AUTO_DONE */
	if (reg_cdc_status & CS42L42_HSDET_AUTO_DONE) {

		type = ((reg_hs_status & CS42L42_HSTYPE_MASK) + 1);
		/* CS42L42 reports optical jack as type 4
		 * We don't handle optical jack
		 */
		if (type != 4) {
			if (!spec->cs42l42_hp_jack_in) {
				status_changed = 1;
				spec->cs42l42_hp_jack_in = 1;
			}
			/* type = 3 has no mic */
			if ((!spec->cs42l42_mic_jack_in) && (type != 3)) {
				status_changed = 1;
				spec->cs42l42_mic_jack_in = 1;
			}
		} else {
			if (spec->cs42l42_hp_jack_in || spec->cs42l42_mic_jack_in) {
				status_changed = 1;
				spec->cs42l42_hp_jack_in = 0;
				spec->cs42l42_mic_jack_in = 0;
			}
		}

	} else {
		/* TIP_SENSE INSERT/REMOVE */
		switch (reg_ts_status) {
		case CS42L42_JACK_INSERTED:
			cs8409_cs42l42_run_jack_detect(codec);
			break;

		case CS42L42_JACK_REMOVED:
			if (spec->cs42l42_hp_jack_in || spec->cs42l42_mic_jack_in) {
				status_changed = 1;
				spec->cs42l42_hp_jack_in = 0;
				spec->cs42l42_mic_jack_in = 0;
			}
			break;

		default:
			/* jack in transition */
			status_changed = 0;
			break;
		}
	}

	if (status_changed) {

		snd_hda_set_pin_ctl(codec, CS8409_CS42L42_SPK_PIN_NID,
				spec->cs42l42_hp_jack_in ? 0 : PIN_OUT);

		/* Report jack*/
		jk = snd_hda_jack_tbl_get_mst(codec, CS8409_CS42L42_HP_PIN_NID, 0);
		if (jk) {
			snd_hda_jack_unsol_event(codec,
				(jk->tag << AC_UNSOL_RES_TAG_SHIFT) & AC_UNSOL_RES_TAG);
		}
		/* Report jack*/
		jk = snd_hda_jack_tbl_get_mst(codec, CS8409_CS42L42_AMIC_PIN_NID, 0);
		if (jk) {
			snd_hda_jack_unsol_event(codec,
				(jk->tag << AC_UNSOL_RES_TAG_SHIFT) & AC_UNSOL_RES_TAG);
		}
	}
}

#ifdef CONFIG_PM
/* Manage PDREF, when transition to D3hot */
static int cs8409_suspend(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	mutex_lock(&spec->cs8409_i2c_mux);
	/* Power down CS42L42 ASP/EQ/MIX/HP */
	cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x1101, 0xfe, 1);
	mutex_unlock(&spec->cs8409_i2c_mux);
	/* Assert CS42L42 RTS# line */
	snd_hda_codec_write(codec,
			codec->core.afg, 0, AC_VERB_SET_GPIO_DATA, 0);

	snd_hda_shutup_pins(codec);

	return 0;
}
#endif

/* Enable/Disable Unsolicited Response for gpio(s) 3,4 */
static void cs8409_enable_ur(struct hda_codec *codec, int flag)
{
	/* GPIO4 INT# and GPIO3 WAKE# */
	snd_hda_codec_write(codec, codec->core.afg,
			0, AC_VERB_SET_GPIO_UNSOLICITED_RSP_MASK,
			flag ? (GPIO3_INT | GPIO4_INT) : 0);

	snd_hda_codec_write(codec, codec->core.afg,
			0, AC_VERB_SET_UNSOLICITED_ENABLE,
			flag ? AC_UNSOL_ENABLED : 0);

}

/* Vendor specific HW configuration
 * PLL, ASP, I2C, SPI, GPIOs, DMIC etc...
 */
static void cs8409_cs42l42_hw_init(struct hda_codec *codec)
{
	const struct cs8409_cir_param *seq = cs8409_cs42l42_hw_cfg;
	const struct cs8409_cir_param *seq_bullseye = cs8409_cs42l42_bullseye_atn;
	struct cs_spec *spec = codec->spec;

	if (spec->gpio_mask) {
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_MASK,
				    spec->gpio_mask);
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DIRECTION,
				    spec->gpio_dir);
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DATA,
				    spec->gpio_data);
	}

	for (; seq->nid; seq++)
		cs_vendor_coef_set(codec, seq->cir, seq->coeff);

	if (codec->fixup_id == CS8409_BULLSEYE)
		for (; seq_bullseye->nid; seq_bullseye++)
			cs_vendor_coef_set(codec, seq_bullseye->cir, seq_bullseye->coeff);

	/* Disable Unsolicited Response during boot */
	cs8409_enable_ur(codec, 0);

	/* Reset CS42L42 */
	cs8409_cs42l42_reset(codec);

	/* Initialise CS42L42 companion codec */
	cs8409_cs42l42_reg_setup(codec);

	if (codec->fixup_id == CS8409_WARLOCK ||
			codec->fixup_id == CS8409_CYBORG) {
		/* FULL_SCALE_VOL = 0 for Warlock / Cyborg */
		mutex_lock(&spec->cs8409_i2c_mux);
		cs8409_i2c_write(codec, CS42L42_I2C_ADDR, 0x2001, 0x01, 1);
		mutex_unlock(&spec->cs8409_i2c_mux);
		/* DMIC1_MO=00b, DMIC1/2_SR=1 */
		cs_vendor_coef_set(codec, 0x09, 0x0003);
	}

	/* Restore Volumes after Resume */
	if (spec->cs42l42_volume_init) {
		mutex_lock(&spec->cs8409_i2c_mux);
		cs8409_i2c_write(codec, CS42L42_I2C_ADDR,
					CS8409_CS42L42_REG_HS_VOLUME_CHA,
					-spec->cs42l42_hp_volume[0],
					1);
		cs8409_i2c_write(codec, CS42L42_I2C_ADDR,
					CS8409_CS42L42_REG_HS_VOLUME_CHB,
					-spec->cs42l42_hp_volume[1],
					1);
		cs8409_i2c_write(codec, CS42L42_I2C_ADDR,
					CS8409_CS42L42_REG_AMIC_VOLUME,
					spec->cs42l42_hs_mic_volume[0],
					1);
		mutex_unlock(&spec->cs8409_i2c_mux);
	}

	cs8409_cs42l42_update_volume(codec);

	cs8409_cs42l42_enable_jack_detect(codec);

	/* Enable Unsolicited Response */
	cs8409_enable_ur(codec, 1);
}

static int cs8409_cs42l42_init(struct hda_codec *codec)
{
	int ret = snd_hda_gen_init(codec);

	if (!ret)
		snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_INIT);

	return ret;
}

static const struct hda_codec_ops cs8409_cs42l42_patch_ops = {
	.build_controls = cs_build_controls,
	.build_pcms = snd_hda_gen_build_pcms,
	.init = cs8409_cs42l42_init,
	.free = cs_free,
	.unsol_event = cs8409_jack_unsol_event,
#ifdef CONFIG_PM
	.suspend = cs8409_suspend,
#endif
};

static void cs8409_cs42l42_fixups(struct hda_codec *codec,
				    const struct hda_fixup *fix, int action)
{
	struct cs_spec *spec = codec->spec;
	int caps;

	switch (action) {
	case HDA_FIXUP_ACT_PRE_PROBE:
		snd_hda_add_verbs(codec, cs8409_cs42l42_init_verbs);
		/* verb exec op override */
		spec->exec_verb = codec->core.exec_verb;
		codec->core.exec_verb = cs8409_cs42l42_exec_verb;

		mutex_init(&spec->cs8409_i2c_mux);

		codec->patch_ops = cs8409_cs42l42_patch_ops;

		spec->gen.suppress_auto_mute = 1;
		spec->gen.no_primary_hp = 1;
		spec->gen.suppress_vmaster = 1;

		/* GPIO 5 out, 3,4 in */
		spec->gpio_dir = GPIO5_INT;
		spec->gpio_data = 0;
		spec->gpio_mask = 0x03f;

		spec->cs42l42_hp_jack_in = 0;
		spec->cs42l42_mic_jack_in = 0;

		/* Basic initial sequence for specific hw configuration */
		snd_hda_sequence_write(codec, cs8409_cs42l42_init_verbs);

		/* CS8409 is simple HDA bridge and intended to be used with a remote
		 * companion codec. Most of input/output PIN(s) have only basic
		 * capabilities. NID(s) 0x24 and 0x34 have only OUTC and INC
		 * capabilities and no presence detect capable (PDC) and call to
		 * snd_hda_gen_build_controls() will mark them as non detectable
		 * phantom jacks. However, in this configuration companion codec
		 * CS42L42 is connected to these pins and it has jack detect
		 * capabilities. We have to override pin capabilities,
		 * otherwise they will not be created as input devices.
		 */
		caps = snd_hdac_read_parm(&codec->core, CS8409_CS42L42_HP_PIN_NID,
				AC_PAR_PIN_CAP);
		if (caps >= 0)
			snd_hdac_override_parm(&codec->core,
				CS8409_CS42L42_HP_PIN_NID, AC_PAR_PIN_CAP,
				(caps | (AC_PINCAP_IMP_SENSE | AC_PINCAP_PRES_DETECT)));

		caps = snd_hdac_read_parm(&codec->core, CS8409_CS42L42_AMIC_PIN_NID,
				AC_PAR_PIN_CAP);
		if (caps >= 0)
			snd_hdac_override_parm(&codec->core,
				CS8409_CS42L42_AMIC_PIN_NID, AC_PAR_PIN_CAP,
				(caps | (AC_PINCAP_IMP_SENSE | AC_PINCAP_PRES_DETECT)));

		snd_hda_override_wcaps(codec, CS8409_CS42L42_HP_PIN_NID,
			(get_wcaps(codec, CS8409_CS42L42_HP_PIN_NID) | AC_WCAP_UNSOL_CAP));

		snd_hda_override_wcaps(codec, CS8409_CS42L42_AMIC_PIN_NID,
			(get_wcaps(codec, CS8409_CS42L42_AMIC_PIN_NID) | AC_WCAP_UNSOL_CAP));
		break;
	case HDA_FIXUP_ACT_PROBE:

		/* Set initial DMIC volume to -26 dB */
		snd_hda_codec_amp_init_stereo(codec, CS8409_CS42L42_DMIC_ADC_PIN_NID,
				HDA_INPUT, 0, 0xff, 0x19);
		snd_hda_gen_add_kctl(&spec->gen,
			NULL, &cs8409_cs42l42_hp_volume_mixer);
		snd_hda_gen_add_kctl(&spec->gen,
			NULL, &cs8409_cs42l42_amic_volume_mixer);
		cs8409_cs42l42_hw_init(codec);
		snd_hda_codec_set_name(codec, "CS8409/CS42L42");
		break;
	case HDA_FIXUP_ACT_INIT:
		cs8409_cs42l42_hw_init(codec);
		fallthrough;
	case HDA_FIXUP_ACT_BUILD:
		/* Run jack auto detect first time on boot
		 * after controls have been added, to check if jack has
		 * been already plugged in.
		 * Run immediately after init.
		 */
		cs8409_cs42l42_run_jack_detect(codec);
		usleep_range(100000, 150000);
		break;
	default:
		break;
	}
}

static int cs8409_cs42l42_exec_verb(struct hdac_device *dev,
		unsigned int cmd, unsigned int flags, unsigned int *res)
{
	struct hda_codec *codec = container_of(dev, struct hda_codec, core);
	struct cs_spec *spec = codec->spec;

	unsigned int nid = ((cmd >> 20) & 0x07f);
	unsigned int verb = ((cmd >> 8) & 0x0fff);

	/* CS8409 pins have no AC_PINSENSE_PRESENCE
	 * capabilities. We have to intercept 2 calls for pins 0x24 and 0x34
	 * and return correct pin sense values for read_pin_sense() call from
	 * hda_jack based on CS42L42 jack detect status.
	 */
	switch (nid) {
	case CS8409_CS42L42_HP_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (spec->cs42l42_hp_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;

	case CS8409_CS42L42_AMIC_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (spec->cs42l42_mic_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;

	default:
		break;
	}

	return spec->exec_verb(dev, cmd, flags, res);
}

#if 0
static int patch_cs8409(struct hda_codec *codec)
{
	int err;

	if (!cs_alloc_spec(codec, CS8409_VENDOR_NID))
		return -ENOMEM;

	snd_hda_pick_fixup(codec,
			cs8409_models, cs8409_fixup_tbl, cs8409_fixups);

	codec_dbg(codec, "Picked ID=%d, VID=%08x, DEV=%08x\n",
			codec->fixup_id,
			codec->bus->pci->subsystem_vendor,
			codec->bus->pci->subsystem_device);

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PRE_PROBE);

	err = cs_parse_auto_config(codec);
	if (err < 0) {
		cs_free(codec);
		return err;
	}

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PROBE);
	return 0;
}
#endif

static void cs_8409_pcm_playback_pre_prepare_hook(struct hda_pcm_stream *hinfo, struct hda_codec *codec, 
                               unsigned int stream_tag, unsigned int format, struct snd_pcm_substream *substream,
                               int action);

// this is a copy from playback_pcm_prepare in hda_generic.c
// initially I needed to do the Apple setup BEFORE the snd_hda_multi_out_analog_prepare
// in order to overwrite the Apple setup with the actual format/stream id
// NOTA BENE - if playback_pcm_prepare is changed in hda_generic.c then
// those changes must be re-implemented here
// we need this order because snd_hda_multi_out_analog_prepare writes the
// the format and stream id's to the audio nodes
//// so far we have left the Apple setup of the nodes format and stream id's in
// now updated to set the actual format where Apple does the format/stream id setup
// Apples format is very specifically S24_3LE (24 bit), 4 channel, 44.1 kHz
// S24_3LE seems to be very difficult to create so best Ive done is
// S24_LE (24 in 32 bits) or S32_LE
// it seems the digital setup is able to handle this with the Apple TDM
// setup but if we use the normal prepare hook order this overrwites
// the node linux 0x2, 0x3 setup with the Apple setup which leads to noise
// (the HDA specs say the node format setup must match the data)
// if we do the Apple setup and then the snd_hda_multi_out_analog_prepare
// the nodes will have the slightly different but working format
// with proper update of stream format at same point as in Apple log we need to pass
// the actual playback format as passed to this routine to our new "hook"
// cs_8409_pcm_playback_pre_prepare_hook
// to define the cached format correctly in that routine
// so far my analysis is that hinfo stores the stream format in the kernel format style
// but what is passed to cs_8409_playback_pcm_prepare is the format in HDA style
// not yet figured how to convert from kernel format style to HDA style

static int cs_8409_playback_pcm_prepare(struct hda_pcm_stream *hinfo,
                                struct hda_codec *codec,
                                unsigned int stream_tag,
                                unsigned int format,
                                struct snd_pcm_substream *substream)
{
        struct hda_gen_spec *spec = codec->spec;
        int err;
        codec_dbg(codec, "cs_8409_playback_pcm_prepare\n");

        codec_dbg(codec, "cs_8409_playback_pcm_prepare: NID=0x%x, stream=0x%x, format=0x%x\n",
                  hinfo->nid, stream_tag, format);

        cs_8409_pcm_playback_pre_prepare_hook(hinfo, codec, stream_tag, format, substream,
                               HDA_GEN_PCM_ACT_PREPARE);

        err = snd_hda_multi_out_analog_prepare(codec, &spec->multiout,
                                               stream_tag, format, substream);

	// we cant call directly as call_pcm_playback_hook is local to hda_generic.c
        //if (!err)
        //        call_pcm_playback_hook(hinfo, codec, substream,
        //                               HDA_GEN_PCM_ACT_PREPARE);
	// but its a trivial function - at least for the moment!!
	if (err)
                codec_dbg(codec, "cs_8409_playback_pcm_prepare err %d\n", err);
        if (!err)
                if (spec->pcm_playback_hook)
                        spec->pcm_playback_hook(hinfo, codec, substream, HDA_GEN_PCM_ACT_PREPARE);
        return err;
}


static void cs_8409_pcm_capture_pre_prepare_hook(struct hda_pcm_stream *hinfo, struct hda_codec *codec, 
                               unsigned int stream_tag, unsigned int format, struct snd_pcm_substream *substream,
                               int action);


// this is a copy from capture_pcm_prepare in hda_generic.c
// NOTA BENE - if capture_pcm_prepare is changed in hda_generic.c then
// those changes must be re-implemented here
static int cs_8409_capture_pcm_prepare(struct hda_pcm_stream *hinfo,
                               struct hda_codec *codec,
                               unsigned int stream_tag,
                               unsigned int format,
                               struct snd_pcm_substream *substream)
{
        struct cs_spec *spec = codec->spec;

        codec_dbg(codec, "cs_8409_capture_pcm_prepare\n");

        codec_dbg(codec, "cs_8409_capture_pcm_prepare: NID=0x%x, stream=0x%x, format=0x%x\n",
                  hinfo->nid, stream_tag, format);


        cs_8409_pcm_capture_pre_prepare_hook(hinfo, codec, stream_tag, format, substream,
                              HDA_GEN_PCM_ACT_PREPARE);

	// we have a problem - this has to handle 2 different types of stream - the internal mike
	// and the external headset mike (cs42l83)


	// NOTE - the following snd_hda_codec_stream no longer do anything
	//        we have already set the stream data in the pre prepare hook
	//        - so as the format here is same (or at least should be!!) as that setup there is no format difference to that
	//        cached and snd_hda_coded_setup_stream does nothing

	if (hinfo->nid == spec->intmike_adc_nid)
	{

	// so this is getting stranger and stranger
	// the most valid recording is S24_3LE (0x4031) - except that the data we get out is S32_LE (low byte 0)
	// - so it doesnt play right - and it messes with arecords vumeter
	// (S32_LE is officially 0x4041 - but using that format doesnt seem to have valid data - audio very low)
	//// so now try forcing the format here to 0x4031
	//// well that fails miserably - the format mismatch stops data totally
	// it now appears we get the same data with either 0x4031 or 0x4041 - both are low volume
	// - however scaling (normalizing) in audacity we get the right sound with similar quality to OSX
	// so now think the low volume is right - and OSX must be scaling/processing the data in CoreAudio
	// (is the internal mike a fake 24 bits - ie its actually 16 bits but stuffed in the low end of the
	//  24 bits - hence low volume - preliminary scaling attempts in audacity suggest this might be true!!)

        snd_hda_codec_setup_stream(codec, hinfo->nid, stream_tag, 0, format);

	}
	else if (hinfo->nid == 0x1a)
	{

	// do we need a pre-prepare function??
	// maybe for this the external mike ie cs42l83 input

        snd_hda_codec_setup_stream(codec, hinfo->nid, stream_tag, 0, format);

	}
	else
		dev_info(hda_codec_dev(codec), "cs_8409_capture_pcm_prepare - UNIMPLEMENTED input nid 0x%x\n",hinfo->nid);

	// we cant call directly as call_pcm_capture_hook is local to hda_generic.c
        //call_pcm_capture_hook(hinfo, codec, substream,
        //                      HDA_GEN_PCM_ACT_PREPARE);
	// but its a trivial function - at least for the moment!!
	// note this hook if defined also needs to switch between the 2 versions of input!!
        if (spec->gen.pcm_capture_hook)
                spec->gen.pcm_capture_hook(hinfo, codec, substream, HDA_GEN_PCM_ACT_PREPARE);

        return 0;
}



// another copied routine as this is local to hda_jack.c
static struct hda_jack_tbl *
cs_8409_hda_jack_tbl_new(struct hda_codec *codec, hda_nid_t nid)
{
        struct hda_jack_tbl *jack = snd_hda_jack_tbl_get(codec, nid);
        if (jack)
                return jack;
        jack = snd_array_new(&codec->jacktbl);
        if (!jack)
                return NULL;
        jack->nid = nid;
        jack->jack_dirty = 1;
        jack->tag = codec->jacktbl.used;
	// use this to prevent f09 verbs being sent - not seen in OSX logs
        jack->phantom_jack = 1;
        return jack;
}

// copy of snd_hda_jack_detect_enable_callback code
// there is no AC_VERB_SET_UNSOLICITED_ENABLE for 8409
// it appears unsolicited response is pre-enabled
// but we need to fix this to setup the callback on such responses
struct hda_jack_callback *
cs_8409_hda_jack_detect_enable_callback(struct hda_codec *codec, hda_nid_t nid, int tag,
				    hda_jack_callback_fn func)
{
	struct hda_jack_tbl *jack;
	struct hda_jack_callback *callback = NULL;
	int err;

	jack = cs_8409_hda_jack_tbl_new(codec, nid);
	if (!jack)
		return ERR_PTR(-ENOMEM);
	if (func) {
		callback = kzalloc(sizeof(*callback), GFP_KERNEL);
		if (!callback)
			return ERR_PTR(-ENOMEM);
		callback->func = func;
		callback->nid = jack->nid;
		callback->next = jack->callback;
		jack->callback = callback;
	}

	if (jack->jack_detect)
		return callback; /* already registered */
	jack->jack_detect = 1;
	// update the tag - linux code just counted the number of jacks set up
	// for a tag
	// jack->tag = codec->jacktbl.used;
	jack->tag = tag;
	if (codec->jackpoll_interval > 0)
		return callback; /* No unsol if we're polling instead */
	// apparently we dont need to send this
	//err = snd_hda_codec_write_cache(codec, nid, 0,
	//				 AC_VERB_SET_UNSOLICITED_ENABLE,
	//				 AC_USRSP_EN | jack->tag);
	//if (err < 0)
	//	return ERR_PTR(err);
	return callback;
}

#ifdef ADD_EXTENDED_VERB
static void cs_8409_set_extended_codec_verb(void);
#endif

static int cs_8409_init(struct hda_codec *codec)
{
	struct hda_pcm *info = NULL;
	struct hda_pcm_stream *hinfo = NULL;
	struct cs_spec *spec = NULL;
	//struct snd_kcontrol *kctl = NULL;
	int pcmcnt = 0;
	int ret_unsol_enable = 0;

	// so apparently if we do not define a resume function
	// then this init function will be called on resume
	// is that what we want here??
	// NOTE this is called for either playback or capture

        myprintk("snd_hda_intel: cs_8409_init\n");

	//if (spec->vendor_nid == CS420X_VENDOR_NID) {
	//	/* init_verb sequence for C0/C1/C2 errata*/
	//	snd_hda_sequence_write(codec, cs_errata_init_verbs);
	//	snd_hda_sequence_write(codec, cs_coef_init_verbs);
	//} else if (spec->vendor_nid == CS4208_VENDOR_NID) {
	//	snd_hda_sequence_write(codec, cs4208_coef_init_verbs);
	//}


	//// so it looks as tho we have an issue when using headsets
	//// - because the 8409 is totally messed up it does not switch the inputs
	//// when a headset is plugged in
	//// not sure about this here - maybe move to where disable internal mike nodes
	//if (spec->jack_present) {
	//}


	// so the following powers on all active nodes - but if we have just plugged
	// in a headset thats still the internal mike and amps

	snd_hda_gen_init(codec);

	// dump the rates/format of the afg node
	// so analog_playback_stream is still NULL here - maybe only defined when doing actual playback
	// the info stream is now defined
	spec = codec->spec;
        hinfo = spec->gen.stream_analog_playback;
	if (hinfo != NULL)
	{
		codec_dbg(codec, "hinfo stream nid 0x%02x rates 0x%08x formats 0x%016llx\n",hinfo->nid,hinfo->rates,hinfo->formats);
	}
	else
		codec_dbg(codec, "hinfo stream NULL\n");

	// think this is what I need to fixup

        list_for_each_entry(info, &codec->pcm_list_head, list) {
                int stream;

                codec_dbg(codec, "cs_8409_init pcm %d\n",pcmcnt);

                for (stream = 0; stream < 2; stream++) {
                        struct hda_pcm_stream *hinfo = &info->stream[stream];

			codec_dbg(codec, "cs_8409_init info stream %d pointer %p\n",stream,hinfo);

			if (hinfo != NULL)
			{
				codec_dbg(codec, "cs_8409_init info stream %d nid 0x%02x rates 0x%08x formats 0x%016llx\n",stream,hinfo->nid,hinfo->rates,hinfo->formats);
				codec_dbg(codec, "cs_8409_init        stream substreams %d\n",hinfo->substreams);
				codec_dbg(codec, "cs_8409_init        stream channels min %d\n",hinfo->channels_min);
				codec_dbg(codec, "cs_8409_init        stream channels max %d\n",hinfo->channels_max);
				codec_dbg(codec, "cs_8409_init        stream maxbps %d\n",hinfo->maxbps);
			}
			else
				codec_dbg(codec, "cs_8409_init info stream %d NULL\n", stream);
		}
		pcmcnt++;
	}

	// update the streams specifically by nid
	// we seem to have only 1 stream here with the nid of 0x02
	// (I still dont really understand the linux generic coding here)
	// with capture devices we seem to get 2 pcm streams (0 and 1)
	// each pcm stream has an output stream (0) and an input stream (1)
	// the 1st pcm stream (0) is assigned nid 0x02 for output and nid 0x22 (macbook pro) for input (internal mike)
	// the 2nd pcm stream (1) has a dummy output stream and nid 0x1a for input (headset mike via cs42l83)
	// (NOTE this means the line input stream (0x45->0x23) (macbook pro) is not assigned currently ie not useable)

        list_for_each_entry(info, &codec->pcm_list_head, list) {
                int stream;

                for (stream = 0; stream < 2; stream++) {
                        struct hda_pcm_stream *hinfo = &info->stream[stream];

			if (hinfo != NULL)
			{
				if (stream == SNDRV_PCM_STREAM_PLAYBACK)
				{
					if (hinfo->nid == 0x02)
					{
						codec_dbg(codec, "cs_8409_init info playback stream %d pointer %p\n",stream,hinfo);
						// so now we need to force the rates and formats to the single one Apple defines ie 44.1 kHz and S24_LE
						// probably can leave S32_LE
						// we can still handle 2/4 channel (what about 1 channel?)
						hinfo->rates = SNDRV_PCM_RATE_44100;
						hinfo->formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S24_LE;
						codec_dbg(codec, "playback info stream forced nid 0x%02x rates 0x%08x formats 0x%016llx\n",hinfo->nid,hinfo->rates,hinfo->formats);

						// update the playback function
						hinfo->ops.prepare = cs_8409_playback_pcm_prepare;
					}
				}
				else if (stream == SNDRV_PCM_STREAM_CAPTURE)
				{
					//if (hinfo->nid == 0x22)
					if (hinfo->nid == spec->intmike_adc_nid)
					{
						// this is the internal mike
						// this is a bit weird - the output nodes are id'ed by input pin nid
						// but the input nodes are done by the input (adc) nid - not the input pin nid
						codec_dbg(codec, "cs_8409_init info capture stream %d pointer %p\n",stream,hinfo);
						// so now we could force the rates and formats to the single one Apple defines ie 44.1 kHz and S24_LE
						// but this internal mike seems to be a standard HDA input setup so we could have any format here
						//hinfo->rates = SNDRV_PCM_RATE_44100;
						//hinfo->formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S24_LE;
						hinfo->rates = SNDRV_PCM_RATE_44100;
						//hinfo->formats = SNDRV_PCM_FMTBIT_S24_3LE;
						hinfo->formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_3LE;
						//hinfo->maxbps = 24;
						codec_dbg(codec, "capture info stream forced nid 0x%02x rates 0x%08x formats 0x%016llx maxbps %d\n",hinfo->nid,hinfo->rates,hinfo->formats,hinfo->maxbps);
						// update the capture function
						hinfo->ops.prepare = cs_8409_capture_pcm_prepare;
					}
					else if (hinfo->nid == 0x1a)
					{
						// this is the external mike ie headset mike
						// this is a bit weird - the output nodes are id'ed by input pin nid
						// but the input nodes are done by the input (adc) nid - not the input pin nid
						codec_dbg(codec, "cs_8409_init info capture stream %d pointer %p\n",stream,hinfo);
						// so now we force the rates and formats to the single one Apple defines ie 44.1 kHz and S24_LE
						// - because this format is the one being returned by the cs42l83 which is setup by undocumented i2c commands
						hinfo->rates = SNDRV_PCM_RATE_44100;
						//hinfo->formats = SNDRV_PCM_FMTBIT_S24_LE;
						hinfo->formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_3LE;
						//hinfo->maxbps = 24;
						codec_dbg(codec, "capture info stream forced nid 0x%02x rates 0x%08x formats 0x%016llx maxbps %d\n",hinfo->nid,hinfo->rates,hinfo->formats,hinfo->maxbps);
						// update the capture function
						hinfo->ops.prepare = cs_8409_capture_pcm_prepare;
					}
					// still not sure what we do about the linein nid
					// is this bidirectional - because we have no lineout as far as I can see
				}
			}
			else
				codec_dbg(codec, "cs_8409_init info pcm stream %d NULL\n", stream);
		}
		pcmcnt++;
	}


	//list_for_each_entry(kctl, &codec->card->controls, list) {
	//}


	// read UNSOL enable data to see what initial setup is
        //ret_unsol_enable = snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	//codec_dbg(codec,"UNSOL event 0x01 boot setup is 0x%08x\n",ret_unsol_enable);
        //ret_unsol_enable = snd_hda_codec_read(codec, 0x47, 0, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	//codec_dbg(codec,"UNSOL event 0x47 boot setup is 0x%08x\n",ret_unsol_enable);


	//if (spec->gpio_mask) {
	//	snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_MASK,
	//			    spec->gpio_mask);
	//	snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DIRECTION,
	//			    spec->gpio_dir);
	//	snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DATA,
	//			    spec->gpio_data);
	//}

	//if (spec->vendor_nid == CS420X_VENDOR_NID) {
	//	init_input_coef(codec);
	//	init_digital_coef(codec);
	//}

#ifdef ADD_EXTENDED_VERB
	cs_8409_set_extended_codec_verb();
#endif


        myprintk("snd_hda_intel: end cs_8409_init\n");

	return 0;
}

static int cs_8409_build_controls(struct hda_codec *codec)
{
	int err;

        myprintk("snd_hda_intel: cs_8409_build_controls\n");

	err = snd_hda_gen_build_controls(codec);
	if (err < 0)
		return err;
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_BUILD);

        myprintk("snd_hda_intel: end cs_8409_build_controls\n");
	return 0;
}

int cs_8409_build_pcms(struct hda_codec *codec)
{
	int retval;
	//struct cs_spec *spec = codec->spec;
	//struct hda_pcm *info = NULL;
	//struct hda_pcm_stream *hinfo = NULL;
        myprintk("snd_hda_intel: cs_8409_build_pcms\n");
	retval =  snd_hda_gen_build_pcms(codec);
	// we still dont have the pcm streams defined by here
	// ah this is all done in snd_hda_codec_build_pcms
	// which calls this patch routine or snd_hda_gen_build_pcms
	// but the query supported pcms is only done after this
        myprintk("snd_hda_intel: end cs_8409_build_pcms\n");
	return retval;
}


static void cs_8409_call_jack_callback(struct hda_codec *codec,
                               struct hda_jack_tbl *jack)
{
        struct hda_jack_callback *cb;

        for (cb = jack->callback; cb; cb = cb->next)
                cb->func(codec, cb);
        if (jack->gated_jack) {
                struct hda_jack_tbl *gated =
                        snd_hda_jack_tbl_get(codec, jack->gated_jack);
                if (gated) {
                        for (cb = gated->callback; cb; cb = cb->next)
                                cb->func(codec, cb);
                }
        }
}

// so I think this is what gets called for any unsolicited event - including jack plug events
// so anything we do to switch amp/headphone should be done from here

void cs_8409_jack_unsol_event(struct hda_codec *codec, unsigned int res)
{
        struct hda_jack_tbl *event;
        //int ret_unsol_enable = 0;
        int tag = (res >> AC_UNSOL_RES_TAG_SHIFT) & 0x7f;

	//// read UNSOL enable data to see what current setup is
        //ret_unsol_enable = snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	//codec_dbg(codec,"UNSOL event 0x01 at unsol is 0x%08x\n",ret_unsol_enable);
        //ret_unsol_enable = snd_hda_codec_read(codec, 0x47, 0, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	//codec_dbg(codec,"UNSOL event 0x47 at unsol is 0x%08x\n",ret_unsol_enable);

	// so it seems the low order byte of the res for the 8409 is a copy of the GPIO register state
	// - except that we dont seem to pass this to the callback functions!!

        mycodec_info(codec, "cs_8409_jack_unsol_event UNSOL 0x%08x tag 0x%02x\n",res,tag);

        event = snd_hda_jack_tbl_get_from_tag(codec, tag, 0);
        if (!event)
                return;
        event->jack_dirty = 1;

	// its the callback struct thats passed as an argument to the callback function
	// so stuff the res data in the private_data member which seems to be used for such a purpose
        event->callback->private_data = res;

        // leave this as is even tho so far have only 1 tag so not really needed
        // so could just call the callback routine directly here
        cs_8409_call_jack_callback(codec, event);

	// this is the code that generates the 0xf09 verb
	// however if we define the jack as a phantom_jack we do not send the 0xf09 verb
	// we need to call this even tho we only have 1 jack to reset jack_dirty
        snd_hda_jack_report_sync(codec);
}

// Im pretty convinced that Apple uses a timed event from the plugin event
// before performing further setup
// not clear how to set this up in linux
// timer might be way to go but there are some limitations on the timer function
// which is not clear is going to work here
// now think just using msleeps is the way to go - this is similar to code in patch_realtek.c
// for dealing with similar issues
//static struct timer_list cs_8409_hp_timer;

//static void cs_8409_hp_timer_callback(struct timer_list *tlist)
//{
//        myprintk("snd_hda_intel: cs_8409_hp_timer_callback\n");
//}

// have an explict one for 8409
// cs_free is just a definition
//#define cs_8409_free		snd_hda_gen_free

void cs_8409_free(struct hda_codec *codec)
{
	//del_timer(&cs_8409_hp_timer);

	snd_hda_gen_free(codec);
}


// note this must come after any function definitions used

static const struct hda_codec_ops cs_8409_patch_ops = {
	.build_controls = cs_8409_build_controls,
	.build_pcms = cs_8409_build_pcms,
	.init = cs_8409_init,
	.free = cs_8409_free,
	.unsol_event = cs_8409_jack_unsol_event,
};


static int cs_8409_create_input_ctls(struct hda_codec *codec);


static int cs_8409_parse_auto_config(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	int err;
	int i;

        myprintk("snd_hda_intel: cs_8409_parse_auto_config\n");

	err = snd_hda_parse_pin_defcfg(codec, &spec->gen.autocfg, NULL, 0);
	if (err < 0)
		return err;

	err = snd_hda_gen_parse_auto_config(codec, &spec->gen.autocfg);
	if (err < 0)
		return err;

	// note that create_input_ctls is called towards the end of snd_hda_gen_parse_auto_config

	// so it appears the auto config assumes that inputs are connected to ADCs
	// (not true for outputs)

	// I dont really get these - but they dont seem to be useful for the 8409 - seem to allocate nids that are never used
	// they dont seem to be line inputs either
	// well setting num_adc_nids to 0 doesnt work - no inputs defined
	// because it appears the auto config assumes the inputs are connected to an ADC (or audio input converter widget)
	// (NOTE - although these are labelled ADC nodes in the code they may not have an actual analog to digital
	//  converter - may just be a digital sample formatter eg S/PDIF input - for the 8409 the internal mike
	//  seems to be a standard ADC node (eg 0x22 for macbook pro) but the headphone input node (0x1a) is a digital input as digitization
	//  has already occurred in the cs42l83)
	// now recoding the input setup in separate function
	//spec->gen.num_adc_nids = 0;


	// new routine to setup inputs - based on the hda_generic code
	cs_8409_create_input_ctls(codec);


        // so do I keep this or not??
	/* keep the ADCs powered up when it's dynamically switchable */
	if (spec->gen.dyn_adc_switch) {
		unsigned int done = 0;
		for (i = 0; i < spec->gen.input_mux.num_items; i++) {
			int idx = spec->gen.dyn_adc_idx[i];
			if (done & (1 << idx))
				continue;
			snd_hda_gen_fix_pin_power(codec,
						  spec->gen.adc_nids[idx]);
			done |= 1 << idx;
		}
	}

        myprintk("snd_hda_intel: end cs_8409_parse_auto_config\n");

	return 0;
}

// pigs - we need a lot of hda_generic local functions
#include "patch_cirrus_hda_generic_copy.h"

// so we need to hack this code because we have more adcs than AUTO_CFG_MAX_INS
// adcs (8) - actual number is 18
// no good way to do this - except to check connection list for each adc and
// see if connected to nid we are looking at
// so define new function

static int cs_8409_add_adc_nid(struct hda_codec *codec, hda_nid_t pin)
{
	struct hda_gen_spec *spec = codec->spec;
	hda_nid_t nid;
	hda_nid_t *adc_nids = spec->adc_nids;
	int max_nums = ARRAY_SIZE(spec->adc_nids);
	int nums = 0;
	int itm = 0;

        myprintk("snd_hda_intel: cs_8409_add_adc_nid pin 0x%x\n",pin);

	for_each_hda_codec_node(nid, codec) {
		unsigned int caps = get_wcaps(codec, nid);
		int type = get_wcaps_type(caps);
		int fndnid = 0;

		if (type != AC_WID_AUD_IN || (caps & AC_WCAP_DIGITAL))
			continue;

		//myprintk("snd_hda_intel: cs_8409_add_adc_nid nid 0x%x\n",nid);

		{
		const hda_nid_t *connptr = NULL;
		int num_conns = snd_hda_get_conn_list(codec, nid, &connptr);
		int i;
		fndnid = 0;
		for (i = 0; i < num_conns; i++) {
			//myprintk("snd_hda_intel: cs_8409_add_adc_nid %d 0x%x\n",num_conns,connptr[i]);
			if (connptr[i] == pin) {
				fndnid = nid;
			}
		}
		}
		if (fndnid == 0)
			continue;

		// save only 1st one we match
		if (spec->num_adc_nids+1 >= max_nums)
			break;
		adc_nids[spec->num_adc_nids] = nid;
		spec->num_adc_nids += 1;
		break;
	}


	codec_dbg(codec, "snd_hda_intel: cs_8409_add_adc_nid num nids %d\n",nums);

	for (itm = 0; itm < spec->num_adc_nids; itm++) {
		myprintk("snd_hda_intel: cs_8409_add_adc_nid 0x%02x\n", spec->adc_nids[itm]);
	}

	myprintk("snd_hda_intel: end cs_8409_add_adc_nid\n");

	return nums;
}



// copied from parse_capture_source in hda_generic.c
// we need this although not changed (apart from printks) because local to hda_generic.c

/* parse capture source paths from the given pin and create imux items */
static int cs_8409_parse_capture_source(struct hda_codec *codec, hda_nid_t pin,
				int cfg_idx, int num_adcs,
				const char *label, int anchor)
{
	struct hda_gen_spec *spec = codec->spec;
	struct hda_input_mux *imux = &spec->input_mux;
	int imux_idx = imux->num_items;
	bool imux_added = false;
	int c;

	myprintk("snd_hda_intel: cs_8409_parse_capture_source pin 0x%x\n",pin);

	for (c = 0; c < num_adcs; c++) {
		struct nid_path *path;
		hda_nid_t adc = spec->adc_nids[c];

		myprintk("snd_hda_intel: cs_8409_parse_capture_source pin 0x%x adc 0x%x check reachable\n",pin,adc);

		if (!is_reachable_path(codec, pin, adc))
			continue;
		myprintk("snd_hda_intel: cs_8409_parse_capture_source pin 0x%x adc 0x%x reachable\n",pin,adc);
		path = snd_hda_add_new_path(codec, pin, adc, anchor);
		if (!path)
			continue;
		print_nid_path(codec, "input", path);
		spec->input_paths[imux_idx][c] =
			snd_hda_get_path_idx(codec, path);

		if (!imux_added) {
			if (spec->hp_mic_pin == pin)
				spec->hp_mic_mux_idx = imux->num_items;
			spec->imux_pins[imux->num_items] = pin;
			snd_hda_add_imux_item(codec, imux, label, cfg_idx, NULL);
			imux_added = true;
			if (spec->dyn_adc_switch)
				spec->dyn_adc_idx[imux_idx] = c;
		}
	}

        myprintk("snd_hda_intel: end cs_8409_parse_capture_source\n");

	return 0;
}


#define CFG_IDX_MIX	99	/* a dummy cfg->input idx for stereo mix */

// copied from create_input_ctls in hda_generic.c

static int cs_8409_create_input_ctls(struct hda_codec *codec)
{
	struct hda_gen_spec *spec = codec->spec;
	const struct auto_pin_cfg *cfg = &spec->autocfg;
	hda_nid_t mixer = spec->mixer_nid;
	int num_adcs = 0;
	int i, err;
	unsigned int val;

        myprintk("snd_hda_intel: cs_8409_create_input_ctls\n");

	// we cannot do this
	//num_adcs = cs_8409_fill_adc_nids(codec);
	//if (num_adcs < 0)
	//	return 0;

	// clear out the auto config setup
	// hope that all_adcs is not different from adc_nids - doesnt seem to be for auto config only
	memset(spec->adc_nids, 0, sizeof(spec->adc_nids));
	memset(spec->all_adcs, 0, sizeof(spec->all_adcs));
	spec->num_adc_nids = 0;

	for (i = 0; i < cfg->num_inputs; i++) {
		hda_nid_t pin;
		int fndadc = 0;

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d\n",i);

		pin = cfg->inputs[i].pin;
		if (!is_input_pin(codec, pin))
			continue;

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d pin 0x%x\n",i,pin);

		// now scan all nodes for adc nodes and find one connected to this pin
		fndadc = cs_8409_add_adc_nid(codec, pin);
		if (!fndadc)
			continue;
	}

	num_adcs = spec->num_adc_nids;

	/* copy the detected ADCs to all_adcs[] */
	spec->num_all_adcs = spec->num_adc_nids;
	memcpy(spec->all_adcs, spec->adc_nids,  spec->num_adc_nids* sizeof(hda_nid_t));

	err = fill_input_pin_labels(codec);
	if (err < 0)
		return err;

	for (i = 0; i < cfg->num_inputs; i++) {
		hda_nid_t pin;
		int fndadc = 0;

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d\n",i);

		pin = cfg->inputs[i].pin;
		if (!is_input_pin(codec, pin))
			continue;

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d pin 0x%x\n",i,pin);

		//// now scan the adc nodes and find one connected to this pin
		//fndadc = cs_8409_add_adc_nid(codec, pin);
		//if (!fndadc)
		//	continue;

		val = PIN_IN;
		if (cfg->inputs[i].type == AUTO_PIN_MIC)
			val |= snd_hda_get_default_vref(codec, pin);

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d pin 0x%x val 0x%x\n",i,pin,val);

		if (pin != spec->hp_mic_pin &&
		    !snd_hda_codec_get_pin_target(codec, pin))
			set_pin_target(codec, pin, val, false);

		myprintk("snd_hda_intel: cs_8409_create_input_ctls - input %d pin 0x%x val 0x%x mixer 0x%x\n",i,pin,val,mixer);

		if (mixer) {
			if (is_reachable_path(codec, pin, mixer)) {
				err = new_analog_input(codec, i, pin,
						       spec->input_labels[i],
						       spec->input_label_idxs[i],
						       mixer);
				if (err < 0)
					return err;
			}
		}

		// so connections are from the adc nid to the input pin nid
		//{
		//const hda_nid_t conn[256];
		//const hda_nid_t *connptr = conn;
		//int num_conns = snd_hda_get_conn_list(codec, pin, &connptr);
		//int i;
		//myprintk("snd_hda_intel: cs_8409_create_input_ctls pin 0x%x num conn %d\n",pin,num_conns);
		//for (i = 0; i < num_conns; i++) {
		//	myprintk("snd_hda_intel: cs_8409_create_input_ctls pin 0x%x conn 0x%x\n",pin,conn[i]);
		//}
		//}


		// this is the problem routine - this loops over the adcs to do anything
		// so if num_adcs is 0 or none of the adc entries are used this does nothing

		err = cs_8409_parse_capture_source(codec, pin, i, num_adcs,
					   spec->input_labels[i], -mixer);
		if (err < 0)
			return err;

		// comment for the moment as needs lots of other functions
		//if (spec->add_jack_modes) {
		//	err = create_in_jack_mode(codec, pin);
		//	if (err < 0)
		//		return err;
		//}
	}

	/* add stereo mix when explicitly enabled via hint */
	if (mixer && spec->add_stereo_mix_input == HDA_HINT_STEREO_MIX_ENABLE) {
		err = cs_8409_parse_capture_source(codec, mixer, CFG_IDX_MIX, num_adcs,
					   "Stereo Mix", 0);
		if (err < 0)
			return err;
		else
			spec->suppress_auto_mic = 1;
	}

        myprintk("snd_hda_intel: end cs_8409_create_input_ctls\n");

	return 0;
}


/* do I need this for 8409 - I certainly need some gpio patching */
static void cs_8409_fixup_gpio(struct hda_codec *codec,
                               const struct hda_fixup *fix, int action)
{
       myprintk("snd_hda_intel: cs_8409_fixup_gpio\n");

       // allowable states
       // HDA_FIXUP_ACT_PRE_PROBE,
       // HDA_FIXUP_ACT_PROBE,
       // HDA_FIXUP_ACT_INIT,
       // HDA_FIXUP_ACT_BUILD,
       // HDA_FIXUP_ACT_FREE,

       // so inspection suggests no eapd usage on macs - no 0xf0c or 0x70c commands sent

       if (action == HDA_FIXUP_ACT_PRE_PROBE) {
               //struct cs_spec *spec = codec->spec;

               myprintk("snd_hda_intel: cs_8409_fixup_gpio pre probe\n");

               //myprintk("fixup gpio hp=0x%x speaker=0x%x\n", hp_out_mask, speaker_out_mask);
               //spec->gpio_eapd_hp = hp_out_mask;
               //spec->gpio_eapd_speaker = speaker_out_mask;
               //spec->gpio_mask = 0xff;
               //spec->gpio_data =
               //  spec->gpio_dir =
               //  spec->gpio_eapd_hp | spec->gpio_eapd_speaker;
       }
       else if (action == HDA_FIXUP_ACT_PROBE) {
               myprintk("snd_hda_intel: cs_8409_fixup_gpio probe\n");
       }
       else if (action == HDA_FIXUP_ACT_INIT) {
               myprintk("snd_hda_intel: cs_8409_fixup_gpio init\n");
       }
       else if (action == HDA_FIXUP_ACT_BUILD) {
               myprintk("snd_hda_intel: cs_8409_fixup_gpio build\n");
       }
       else if (action == HDA_FIXUP_ACT_FREE) {
               myprintk("snd_hda_intel: cs_8409_fixup_gpio free\n");
       }
       myprintk("snd_hda_intel: end cs_8409_fixup_gpio\n");
}

static void cs_8409_cs42l83_unsolicited_response(struct hda_codec *codec, unsigned int res);

static void cs_8409_cs42l83_callback(struct hda_codec *codec, struct hda_jack_callback *event)
{
	struct cs_spec *spec = codec->spec;

        mycodec_info(codec, "cs_8409_cs42l83_callback\n");

	// so we have confirmed that these unsol responses are not in linux kernel interrupt state
	//if (in_interrupt())
	//	mycodec_info(codec, "cs_8409_cs42l83_callback - INTERRUPT\n");
	//else
	//	mycodec_info(codec, "cs_8409_cs42l83_callback - not interrupt\n");

	// print the stored unsol res which seems to be the GPIO pins state
	mycodec_info(codec, "cs_8409_cs42l83_callback - event private data 0x%08x\n",event->private_data);


	cs_8409_cs42l83_unsolicited_response(codec, event->private_data);


	// now think timers not the way to go
	// patch_realtek.c has to deal with similar issues of plugin, headset detection
	// and just uses msleep calls
	//mod_timer(&cs_8409_hp_timer, jiffies + msecs_to_jiffies(250));

        // the delayed_work feature might be a way to go tho

        mycodec_info(codec, "cs_8409_cs42l83_callback end\n");
}


// dont know how to handle the headphone plug in/out yet
// unfortunately Im guessing these are based on the HDA spec pin event operation
// and not sure how to trigger the pin events from the logged OSX code of plug in/out events
// ah - the HDA spec says a jack plug event triggers an unsolicted response
// plus sets presence detect bits read by command 0xf09
// we have 4 automute hooks
// void (*automute_hook)(struct hda_codec *codec);
// void (*hp_automute_hook)(struct hda_codec *codec, struct hda_jack_callback *cb);
// void (*line_automute_hook)(struct hda_codec *codec, struct hda_jack_callback *cb);
// void (*mic_autoswitch_hook)(struct hda_codec *codec, struct hda_jack_callback *cb);

static void cs_8409_automute(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	dev_info(hda_codec_dev(codec), "cs_8409_automute called\n");
}

static int cs_8409_boot_setup(struct hda_codec *codec);


static void cs_8409_playback_pcm_hook(struct hda_pcm_stream *hinfo,
                                      struct hda_codec *codec,
                                      struct snd_pcm_substream *substream,
                                      int action);

static void cs_8409_capture_pcm_hook(struct hda_pcm_stream *hinfo,
                                     struct hda_codec *codec,
                                     struct snd_pcm_substream *substream,
                                     int action);



static int patch_cs8409(struct hda_codec *codec)
{
        struct cs_spec *spec;
        int err;
        int itm;
        //hda_nid_t *dac_nids_ptr = NULL;

        int explicit = 0;

        //struct hda_pcm *info = NULL;
        //struct hda_pcm_stream *hinfo = NULL;

        myprintk("snd_hda_intel: Patching for CS8409 explicit %d\n", explicit);
        //mycodec_info(codec, "Patching for CS8409 %d\n", explicit);

        //dump_stack();

        spec = cs_alloc_spec(codec, CS8409_VENDOR_NID);
        if (!spec)
                return -ENOMEM;

        spec->beep_nid = CS8409_BEEP_NID;

        spec->use_data = 0;

        if (explicit)
               {
               //codec->patch_ops = cs_8409_patch_ops_explicit;
               }
        else
               codec->patch_ops = cs_8409_patch_ops;

        spec->gen.pcm_playback_hook = cs_8409_playback_pcm_hook;

        spec->gen.pcm_capture_hook = cs_8409_capture_pcm_hook;

        spec->gen.automute_hook = cs_8409_automute;

        // so it appears we need to explicitly apply pre probe fixups here
        // note that if the pinconfigs lists are empty the pin config fixup
        // is effectively ignored

        //myprintk("cs8409 - 1\n");
        //snd_hda_pick_fixup(codec, cs8409_models, cs8409_fixup_tbl,
        //                   cs8409_fixups);
        //myprintk("cs8409 - 2\n");
        //snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PRE_PROBE);


        //timer_setup(&cs_8409_hp_timer, cs_8409_hp_timer_callback, 0);

        myprintk("snd_hda_intel: cs 8409 jack used %d\n",codec->jacktbl.used);

        // use this to cause unsolicited responses to be stored
        // but not run
        spec->block_unsol = 0;

        INIT_LIST_HEAD(&spec->unsol_list);

        for (itm=0; itm<10; itm++)
                { spec->unsol_items_prealloc_used[itm] = 0; }


        // for the moment set initial jack status to not present
        // we will detect if have jack plugged in on boot later
        spec->jack_present = 0;


        spec->headset_type = 0;

        spec->have_mike = 0;

        spec->have_buttons = 0;

        spec->playing = 0;
        spec->capturing = 0;

        spec->headset_play_format_setup_needed = 1;
        spec->headset_capture_format_setup_needed = 1;

        spec->headset_presetup_done = 0;


        // use this to distinguish which unsolicited phase we are in
        // for the moment - we only seem to get a tag of 0x37 and dont see any
        // different tags being setup in OSX logs
        spec->headset_phase = 0;

        spec->headset_enable = 0;


        // setup the intmike and linein nids
        if (codec->core.subsystem_id == 0x106b1000)
        {
                spec->intmike_nid = 0x45;
                spec->intmike_adc_nid = 0x23;
                spec->linein_nid = 0x44;
                spec->linein_amp_nid = 0x22;
        }
        else
        {
                spec->intmike_nid = 0x44;
                spec->intmike_adc_nid = 0x22;
                spec->linein_nid = 0x45;
                spec->linein_amp_nid = 0x23;
        }


        // so it appears we dont get interrupts in the auto config stage

        // we need to figure out how to setup the jack detect callback
        // not clear what nid should be used - 0x01 or 0x47
        // added a tag argument because we seem to get a tag
        // so far the tag seems to be 0x37
        cs_8409_hda_jack_detect_enable_callback(codec, 0x01, 0x37, cs_8409_cs42l83_callback);

        myprintk("snd_hda_intel: cs 8409 jack used callback %d\n",codec->jacktbl.used);


        //      cs8409_pinmux_init(codec);

       if (!explicit)
       {

              myprintk("snd_hda_intel: pre cs_8409_parse_auto_config\n");

              err = cs_8409_parse_auto_config(codec);
              if (err < 0)
                      goto error;

              myprintk("snd_hda_intel: post cs_8409_parse_auto_config\n");
       }

       // dump headphone config
       myprintk("snd_hda_intel: headphone config hp_jack_present %d\n",spec->gen.hp_jack_present);
       myprintk("snd_hda_intel: headphone config line_jack_present %d\n",spec->gen.line_jack_present);
       myprintk("snd_hda_intel: headphone config speaker_muted %d\n",spec->gen.speaker_muted);
       myprintk("snd_hda_intel: headphone config line_out_muted %d\n",spec->gen.line_out_muted);
       myprintk("snd_hda_intel: headphone config auto_mic %d\n",spec->gen.auto_mic);
       myprintk("snd_hda_intel: headphone config automute_speaker %d\n",spec->gen.automute_speaker);
       myprintk("snd_hda_intel: headphone config automute_lo %d\n",spec->gen.automute_lo);
       myprintk("snd_hda_intel: headphone config detect_hp %d\n",spec->gen.detect_hp);
       myprintk("snd_hda_intel: headphone config detect_lo %d\n",spec->gen.detect_lo);
       myprintk("snd_hda_intel: headphone config keep_vref_in_automute %d\n",spec->gen.keep_vref_in_automute);
       myprintk("snd_hda_intel: headphone config line_in_auto_switch %d\n",spec->gen.line_in_auto_switch);
       myprintk("snd_hda_intel: headphone config auto_mute_via_amp %d\n",spec->gen.auto_mute_via_amp);
       myprintk("snd_hda_intel: headphone config suppress_auto_mute %d\n",spec->gen.suppress_auto_mute);
       myprintk("snd_hda_intel: headphone config suppress_auto_mic %d\n",spec->gen.suppress_auto_mic);

       myprintk("snd_hda_intel: headphone config hp_mic %d\n",spec->gen.hp_mic);

       myprintk("snd_hda_intel: headphone config suppress_hp_mic_detect %d\n",spec->gen.suppress_hp_mic_detect);


       myprintk("snd_hda_intel: auto config pins line_outs %d\n", spec->gen.autocfg.line_outs);
       myprintk("snd_hda_intel: auto config pins line_outs 0x%02x\n", spec->gen.autocfg.line_out_pins[0]);
       myprintk("snd_hda_intel: auto config pins line_outs 0x%02x\n", spec->gen.autocfg.line_out_pins[1]);
       myprintk("snd_hda_intel: auto config pins speaker_outs %d\n", spec->gen.autocfg.speaker_outs);
       myprintk("snd_hda_intel: auto config pins speaker_outs 0x%02x\n", spec->gen.autocfg.speaker_pins[0]);
       myprintk("snd_hda_intel: auto config pins speaker_outs 0x%02x\n", spec->gen.autocfg.speaker_pins[1]);
       myprintk("snd_hda_intel: auto config pins hp_outs %d\n", spec->gen.autocfg.hp_outs);
       myprintk("snd_hda_intel: auto config pins hp_outs 0x%02x\n", spec->gen.autocfg.hp_pins[0]);
       myprintk("snd_hda_intel: auto config pins inputs %d\n", spec->gen.autocfg.num_inputs);

       myprintk("snd_hda_intel: auto config pins inputs  pin 0x%02x\n", spec->gen.autocfg.inputs[0].pin);
       myprintk("snd_hda_intel: auto config pins inputs type %d\n", spec->gen.autocfg.inputs[0].type);
       myprintk("snd_hda_intel: auto config pins inputs is head set mic %d\n", spec->gen.autocfg.inputs[0].is_headset_mic);
       myprintk("snd_hda_intel: auto config pins inputs is head phn mic %d\n", spec->gen.autocfg.inputs[0].is_headphone_mic);
       myprintk("snd_hda_intel: auto config pins inputs is        boost %d\n", spec->gen.autocfg.inputs[0].has_boost_on_pin);

       myprintk("snd_hda_intel: auto config pins inputs  pin 0x%02x\n", spec->gen.autocfg.inputs[1].pin);
       myprintk("snd_hda_intel: auto config pins inputs type %d\n", spec->gen.autocfg.inputs[1].type);
       myprintk("snd_hda_intel: auto config pins inputs is head set mic %d\n", spec->gen.autocfg.inputs[1].is_headset_mic);
       myprintk("snd_hda_intel: auto config pins inputs is head phn mic %d\n", spec->gen.autocfg.inputs[1].is_headphone_mic);
       myprintk("snd_hda_intel: auto config pins inputs is        boost %d\n", spec->gen.autocfg.inputs[1].has_boost_on_pin);

       myprintk("snd_hda_intel: auto config inputs num_adc_nids %d\n", spec->gen.num_adc_nids);
       for (itm = 0; itm < spec->gen.num_adc_nids; itm++) {
               myprintk("snd_hda_intel: auto config inputs adc_nids 0x%02x\n", spec->gen.adc_nids[itm]);
       }

       myprintk("snd_hda_intel: auto config multiout is num_dacs %d\n", spec->gen.multiout.num_dacs);
       for (itm = 0; itm < spec->gen.multiout.num_dacs; itm++) {
               myprintk("snd_hda_intel: auto config multiout is    dac_nids 0x%02x\n", spec->gen.multiout.dac_nids[itm]);
       }

       myprintk("snd_hda_intel: auto config multiout is      hp_nid 0x%02x\n", spec->gen.multiout.hp_nid);

       for (itm = 0; itm < ARRAY_SIZE(spec->gen.multiout.hp_out_nid); itm++) {
               if (spec->gen.multiout.hp_out_nid[itm])
                       myprintk("snd_hda_intel: auto config multiout is  hp_out_nid 0x%02x\n", spec->gen.multiout.hp_out_nid[itm]);
       }
       for (itm = 0; itm < ARRAY_SIZE(spec->gen.multiout.extra_out_nid); itm++) {
               if (spec->gen.multiout.extra_out_nid[itm])
                       myprintk("snd_hda_intel: auto config multiout is xtr_out_nid 0x%02x\n", spec->gen.multiout.extra_out_nid[itm]);
       }

       myprintk("snd_hda_intel: auto config multiout is dig_out_nid 0x%02x\n", spec->gen.multiout.dig_out_nid);
       //myprintk("snd_hda_intel: auto config multiout is slv_dig_out %p\n", spec->gen.multiout.slave_dig_outs);


       // dump the rates/format of the afg node
       // still havent figured out how the user space gets the allowed formats
       // ah - may have figured this
       // except that at this point this is NULL - we need to be after build pcms
       //info = spec->gen.pcm_rec[0];
       //if (info != NULL)
       //{
       //       hinfo = &(info->stream[SNDRV_PCM_STREAM_PLAYBACK]);
       //       if (hinfo != NULL)
       //              codec_dbg(codec, "playback info stream nid 0x%02x rates 0x%08x formats 0x%016llx\n",hinfo->nid,hinfo->rates,hinfo->formats);
       //       else
       //              codec_dbg(codec, "playback info stream NULL\n");
       //}
       //else
       //       codec_dbg(codec, "playback info NULL\n");


       // try removing the unused nodes
       //spec->gen.autocfg.line_outs = 0;
       //spec->gen.autocfg.hp_outs = 0;

       // I dont really get these - but they dont seem to be useful for the 8409 - seem to allocate nids that are never used
       // they dont seem to be line inputs either
       // well setting num_adc_nids to 0 doesnt work - no inputs defined
       // - because all input pin nodes need to be connected to an audio input converter node
       // - which in the hda_generic.c code are labelled as adc nodes/nids
       // now recoding the input setup in separate function
       //spec->gen.num_adc_nids = 0;

       // these seem to be the primary mike inputs ? maybe line inputs??
       //spec->gen.autocfg.num_inputs = 0;

       // to clobber the headphone output we would need to clear the hp_out_nid array
       //spec->gen.multiout.hp_out_nid[0] = 0x00;
       // do this to prevent copying to other streams
       // well this clobbers output!!
       //spec->gen.multiout.no_share_stream = 1;

       // see if using 0x03 only works
       // difficult - apparently dac_nids is a pointer to an array
       // and the spec struct is a const - so we cant change array elements
       // but we can change the pointer to a new list
       // - although we need to update the array elements
       // BEFORE changing the spec pointer - this is rather stupid
       // because we STILL cant update the array elements as an item of the struct
       // maybe if I copied the pointer to a local variable I could update the elements
       // yes that works - because the const qualifier is ignored
       //spec->gen.multiout.num_dacs = 1;
       //spec->gen.multiout.dac_nids = spec->cs_8409_dac_nids;
       //dac_nids_ptr = spec->gen.multiout.dac_nids;
       //dac_nids_ptr[0] = 0x03;
       //dac_nids_ptr[1] = 0x00;
       //spec->gen.multiout.dac_nids[0] = 0x03;
       //spec->gen.multiout.dac_nids[1] = 0x00;


       myprintk("snd_hda_intel: cs 8409 jack used post %d\n",codec->jacktbl.used);


       err = cs_8409_boot_setup(codec);
       if (err < 0)
               goto error;

       // update the headset phase
       spec->headset_phase = 1;

       spec->play_init = 0;
       spec->capture_init = 0;

       // init the last play time
       ktime_get_real_ts64(&(spec->last_play_time));

       ktime_get_real_ts64(&(spec->first_play_time));

       myprintk("snd_hda_intel: Post Patching for CS8409\n");
       //mycodec_info(codec, "Post Patching for CS8409\n");

       return 0;

 error:
       cs_free(codec);
       return err;
}


// for the moment split the new code into an include file

#include "patch_cirrus_new84.h"


// new function to use "vendor" defined commands to run
// a specific code
// has to be here to use functions defined in patch_cirrus_new84.h

static unsigned int
cs_8409_extended_codec_verb(struct hda_codec *codec, hda_nid_t nid,
                                int flags,
                                unsigned int verb, unsigned int parm)
{
	//static inline unsigned int cs_8409_vendor_i2cRead(struct hda_codec *codec, unsigned int i2c_address,
	//                                    unsigned int i2c_reg, unsigned int paged)
	unsigned int retval1 = 0;
	unsigned int retval2 = 0;
	unsigned int retval3 = 0;
	unsigned int retval4 = 0;
	unsigned int retval = 0;

        myprintk("snd_hda_intel: cs_8409_extended_codec_verb nid 0x%02x flags 0x%x verb 0x%03x parm 0x%04x\n", nid, flags, verb, parm);

	if ((verb & 0x0ff8) == 0xf78)
	{
		retval1 = cs_8409_vendor_i2cWrite(codec, 0x64, 0x2d, parm, 0);
		retval2 = cs_8409_vendor_i2cWrite(codec, 0x62, 0x2d, parm, 0);
		retval3 = cs_8409_vendor_i2cWrite(codec, 0x74, 0x2d, parm, 0);
		retval4 = cs_8409_vendor_i2cWrite(codec, 0x72, 0x2d, parm, 0);

		myprintk("snd_hda_intel: cs_8409_extended_codec_verb wr ret 1 0x%x\n",retval1);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb wr ret 2 0x%x\n",retval2);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb wr ret 3 0x%x\n",retval3);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb wr ret 4 0x%x\n",retval4);
	}
	else if ((verb & 0x0ff8) == 0xff8)
	{
		retval1 = cs_8409_vendor_i2cRead(codec, 0x64, 0x2d, 0);
		retval2 = cs_8409_vendor_i2cRead(codec, 0x62, 0x2d, 0);
		retval3 = cs_8409_vendor_i2cRead(codec, 0x74, 0x2d, 0);
		retval4 = cs_8409_vendor_i2cRead(codec, 0x72, 0x2d, 0);

		myprintk("snd_hda_intel: cs_8409_extended_codec_verb rd ret 1 0x%x\n",retval1);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb rd ret 2 0x%x\n",retval2);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb rd ret 3 0x%x\n",retval3);
		myprintk("snd_hda_intel: cs_8409_extended_codec_verb rd ret 4 0x%x\n",retval4);
	}


	retval = retval1;

	return retval;
}

#ifdef ADD_EXTENDED_VERB
static void cs_8409_set_extended_codec_verb(void)
{
	snd_hda_set_extended_codec_verb(cs_8409_extended_codec_verb);
}
#endif


/*
 * patch entries
 */
static const struct hda_device_id snd_hda_id_cirrus[] = {
	HDA_CODEC_ENTRY(0x10134206, "CS4206", patch_cs420x),
	HDA_CODEC_ENTRY(0x10134207, "CS4207", patch_cs420x),
	HDA_CODEC_ENTRY(0x10134208, "CS4208", patch_cs4208),
	HDA_CODEC_ENTRY(0x10134210, "CS4210", patch_cs4210),
	HDA_CODEC_ENTRY(0x10134213, "CS4213", patch_cs4213),
	HDA_CODEC_ENTRY(0x10138409, "CS8409", patch_cs8409),
	{} /* terminator */
};
MODULE_DEVICE_TABLE(hdaudio, snd_hda_id_cirrus);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cirrus Logic HD-audio codec");

static struct hda_codec_driver cirrus_driver = {
	.id = snd_hda_id_cirrus,
};

module_hda_codec_driver(cirrus_driver);
