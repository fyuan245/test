// SPDX-License-Identifier: GPL-2.0-or-later
/*
* HD audio interface patch for Senary HDA audio codec
*
* Initially based on sound/pci/hda/patch_conexant.c
*/

#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <sound/core.h>
#include <sound/jack.h>

#include "hda_codec.h"
#include "hda_local.h"
#include "hda_auto_parser.h"
#include "hda_beep.h"
#include "hda_jack.h"
#include "hda_generic.h"

#define VERSION_MAJOR 		3
#define VERSION_MINOR 		1
#define VERSION_REVISION 	0
#define VERSION_BUILD		1

#define CONFIG_EAPD_POP
#define CONFIG_FIXED_MIC_BOOST

enum {
	SND_HCONTROL_LINEOUT_SWITCH
};

struct senary_spec {
	struct hda_gen_spec gen;

	/* extra EAPD pins */
	unsigned int num_eapds;
	hda_nid_t eapds[4];
	bool dynamic_eapd;
	hda_nid_t mute_led_eapd;
	struct snd_kcontrol* kctls[10];
	struct snd_kcontrol_new* new_kctls[10];
	unsigned int parse_flags; /* flag for snd_hda_parse_pin_defcfg() */

	int mute_led_polarity;
	unsigned int gpio_led;
	unsigned int gpio_mute_led_mask;
	unsigned int gpio_mic_led_mask;
	unsigned char default_mic_boost[0x25];
};

#ifdef CONFIG_SND_HDA_INPUT_BEEP
/* additional beep mixers; private_value will be overwritten */
static const struct snd_kcontrol_new senary_beep_mixer[] = {
	HDA_CODEC_VOLUME_MONO("Beep Playback Volume", 0, 1, 0, HDA_OUTPUT),
	HDA_CODEC_MUTE_BEEP_MONO("Beep Playback Switch", 0, 1, 0, HDA_OUTPUT),
};

static int set_beep_amp(struct senary_spec *spec, hda_nid_t nid,
			int idx, int dir)
{
	struct snd_kcontrol_new *knew;
	unsigned int beep_amp = HDA_COMPOSE_AMP_VAL(nid, 1, idx, dir);
	int i;

	spec->gen.beep_nid = nid;
	for (i = 0; i < ARRAY_SIZE(senary_beep_mixer); i++) {
		knew = snd_hda_gen_add_kctl(&spec->gen, NULL,
						&senary_beep_mixer[i]);
		if (!knew)
			return -ENOMEM;
		knew->private_value = beep_amp;
	}
	return 0;
}

static int senary_auto_parse_beep(struct hda_codec *codec)
{
	struct senary_spec *spec = codec->spec;
	hda_nid_t nid;

	for_each_hda_codec_node(nid, codec)
		if ((get_wcaps_type(get_wcaps(codec, nid)) == AC_WID_BEEP) &&
			(get_wcaps(codec, nid) & (AC_WCAP_OUT_AMP | AC_WCAP_AMP_OVRD)))
			return set_beep_amp(spec, nid, 0, HDA_OUTPUT);
	return 0;
}
#else
#define senary_auto_parse_beep(codec)	0
#endif

/* parse EAPDs */
static void senary_auto_parse_eapd(struct hda_codec *codec)
{
	struct senary_spec *spec = codec->spec;
	hda_nid_t nid;

	for_each_hda_codec_node(nid, codec) {
		if (get_wcaps_type(get_wcaps(codec, nid)) != AC_WID_PIN)
			continue;
		if (!(snd_hda_query_pin_caps(codec, nid) & AC_PINCAP_EAPD))
			continue;
		spec->eapds[spec->num_eapds++] = nid;
		if (spec->num_eapds >= ARRAY_SIZE(spec->eapds))
			break;
	}

	/* NOTE: below is a wild guess; if we have more than two EAPDs,
	* it's a new chip, where EAPDs are supposed to be associated to
	* pins, and we can control EAPD per pin.
	* OTOH, if only one or two EAPDs are found, it's an old chip,
	* thus it might control over all pins.
	*/
	if (spec->num_eapds > 2)
		spec->dynamic_eapd = 1;
}

static void senary_auto_turn_eapd(struct hda_codec *codec, int num_pins,
				const hda_nid_t *pins, bool on)
{
	int i;
	for (i = 0; i < num_pins; i++) {
		if (snd_hda_query_pin_caps(codec, pins[i]) & AC_PINCAP_EAPD)
			snd_hda_codec_write(codec, pins[i], 0,
						AC_VERB_SET_EAPD_BTLENABLE,
						on ? 0x02 : 0);
	}
}

/* turn on/off EAPD according to Master switch */
static void senary_auto_vmaster_hook(void *private_data, int enabled)
{
	struct hda_codec *codec = private_data;
	struct senary_spec *spec = codec->spec;

	senary_auto_turn_eapd(codec, spec->num_eapds, spec->eapds, enabled);
}

static void senary_init_gpio_led(struct hda_codec *codec)
{
	struct senary_spec *spec = codec->spec;
	unsigned int mask = spec->gpio_mute_led_mask | spec->gpio_mic_led_mask;

	if (mask) {
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_MASK,
					mask);
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DIRECTION,
					mask);
		snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DATA,
					spec->gpio_led);
	}
}

void senary_init_verb(struct hda_codec *codec)
{
	snd_hda_codec_write(codec, 0x1b, 0x0, 0x05c, 0x80);
	snd_hda_codec_write(codec, 0x1b, 0x0, 0x071, 0x0a);

	snd_hda_codec_write(codec, 0x1b, 0x0, 0x05a, 0xaa);
	snd_hda_codec_write(codec, 0x1b, 0x0, 0x059, 0x48);
	snd_hda_codec_write(codec, 0x1b, 0x0, 0x01b, 0x00);
	snd_hda_codec_write(codec, 0x1b, 0x0, 0x01c, 0x00);

	snd_hda_codec_write(codec, 0x18, 0x0, 0x370, 0x04);
	snd_hda_codec_write(codec, 0x19, 0x0, 0x370, 0x04);
	snd_hda_codec_write(codec, 0x24, 0x0, 0x370, 0x04);
	snd_hda_codec_write(codec, 0x1a, 0x0, 0x370, 0x04);
}

static int senary_auto_init(struct hda_codec *codec)
{
	struct senary_spec *spec = codec->spec;
	snd_hda_gen_init(codec);
	if (!spec->dynamic_eapd)
		senary_auto_turn_eapd(codec, spec->num_eapds, spec->eapds, true);

	senary_init_gpio_led(codec);
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_INIT);

	return 0;
}

static void senary_auto_shutdown(struct hda_codec *codec)
{
	struct senary_spec *spec = codec->spec;

	/* Turn the problematic codec into D3 to avoid spurious noises
	from the internal speaker during (and after) reboot */
	senary_auto_turn_eapd(codec, spec->num_eapds, spec->eapds, false);
}

static void senary_auto_free(struct hda_codec *codec)
{
	senary_auto_shutdown(codec);
	snd_hda_gen_free(codec);
}

static int senary_hda_gen_build_controls(struct hda_codec *codec)
{
	struct senary_spec *spec = codec->spec;

	int ret = snd_hda_gen_build_controls(codec);

	spec->kctls[SND_HCONTROL_LINEOUT_SWITCH] = snd_hda_find_mixer_ctl(codec, "Line Out Playback Switch");
	if(!spec->kctls[SND_HCONTROL_LINEOUT_SWITCH])
		codec_info(codec, "Line Out Playback Switch Is NULL");

	return ret;
}

static void senary_set_automute(struct hda_codec *codec, hda_nid_t nid, struct snd_ctl_elem_value *ucontrol, bool mute)
{
	struct senary_spec *spec = codec->spec;
	bool enabled ;

	if (mute)
		spec->gen.mute_bits |= (1ULL << nid);
	else
		spec->gen.mute_bits &= ~(1ULL << nid);

	enabled = !((spec->gen.mute_bits >> nid) & 1);

	ucontrol->value.integer.value[0] = enabled;
	ucontrol->value.integer.value[1] = enabled;
}

static int senary_automute_switch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct senary_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);

	struct snd_kcontrol *kctl = spec->kctls[SND_HCONTROL_LINEOUT_SWITCH];
	struct snd_ctl_elem_value *uctl;
	long _switch = *ucontrol->value.integer.value;

	senary_set_automute(codec, nid, ucontrol, !_switch);
	if (kctl)
	{
		uctl = kzalloc(sizeof(*uctl), GFP_KERNEL);
		if (uctl)
		{
			senary_set_automute(codec, 0x11, uctl, _switch);
			kctl->put(kctl, uctl);
			kfree(uctl);
		}
	}

	return snd_hda_mixer_amp_switch_put(kcontrol, ucontrol);
}

static void fixup_senary_automute_switch(struct hda_codec *codec)
{
	struct senary_spec *spec = codec->spec;
	struct snd_kcontrol_new *kctl;
	int i = 0;

	snd_array_for_each(&spec->gen.kctls, i, kctl) {
		if (!strcmp(kctl->name, "Headphone Playback Switch")) {
			kctl->put = senary_automute_switch_put;
			return;
		}
	}
}

#ifdef CONFIG_FIXED_MIC_BOOST
static void senary_fixed_mic_boost(struct hda_codec *codec, unsigned char node_id, unsigned char mic_boost)
{
	unsigned char value = 0;

	value = snd_hda_codec_read(codec, node_id, 0, AC_VERB_GET_AMP_GAIN_MUTE, 0);
	codec_info(codec, "read node_id = %x, value = %x, mic_boost = %x",  node_id, value, mic_boost);

	if(value != mic_boost)
		snd_hda_codec_amp_stereo(codec, node_id, HDA_INPUT, 0, HDA_AMP_VOLMASK, mic_boost);
}

static void senary_cap_sync_hook(struct hda_codec *codec,
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct senary_spec *spec = codec->spec;
	hda_nid_t mux_pin = spec->gen.imux_pins[spec->gen.cur_mux[0]];

	if (spec->default_mic_boost[mux_pin])
		senary_fixed_mic_boost(codec, mux_pin, spec->default_mic_boost[mux_pin]);
	codec_info(codec, "senary_cap_sync_hook mux_pin = %x, mic_boost = %x", mux_pin, spec->default_mic_boost[mux_pin]);
}

void senary_init_fixed_mic_boost(struct hda_codec *codec)
{
	struct senary_spec *spec = codec->spec;
	unsigned char node_ids[4] = {0x18, 0x19, 0x24, 0x1a};
	unsigned char mic_boost;
	int i;

	for(i=0; i<ARRAY_SIZE(node_ids); ++i)
	{
		mic_boost = snd_hda_codec_read(codec, node_ids[i], 0, AC_VERB_GET_AMP_GAIN_MUTE, 0);
		spec->default_mic_boost[node_ids[i]] = mic_boost;
		codec_info(codec, "senary_init_fixed_mic_boost, node_id = %x, mic_boost =%x", node_ids[i], mic_boost);
	}
	spec->gen.cap_sync_hook = senary_cap_sync_hook;
}
#endif

#ifdef CONFIG_PM
static int senary_auto_resume(struct hda_codec *codec)
{
	codec_info(codec, "senary_auto_resume");
	codec->patch_ops.init(codec);
	regcache_sync(codec->core.regmap);
	senary_init_verb(codec);
	return 0;
}
#endif

#ifdef CONFIG_EAPD_POP
void senary_set_port_eapd(struct hda_codec *codec,  unsigned char node_id, bool enable)
{
	unsigned char port_eapd = 0, eapd_value = enable << 1;

	port_eapd = snd_hda_codec_read(codec, node_id, 0, 0xf0c, 0x0);
	if(port_eapd != eapd_value)
	{
		snd_hda_codec_write(codec, node_id, 0,  0x70c, eapd_value);
		codec_info(codec, "senary_set_port_eapd %x70c%x", node_id, eapd_value);
	}
}

static void senary_playback_hook(struct hda_pcm_stream *hinfo, struct hda_codec *codec, struct snd_pcm_substream * substream, int action)
{
	unsigned char eapd = 0;
	struct senary_spec *spec = codec->spec;
	struct auto_pin_cfg *cfg = &spec->gen.autocfg;
	int i;

	switch(action){
		case HDA_GEN_PCM_ACT_PREPARE:
			for(i=0;i < cfg->speaker_outs;++i)
				senary_set_port_eapd(codec, cfg->speaker_pins[i], true);

			eapd = snd_hda_codec_read(codec, 0x1b, 0, 0x84c, 0x0);
			codec_info(codec, "senary_playback_hook eapd = %x", eapd);
			if(eapd)
			{
				snd_hda_codec_write(codec, 0x1b, 0,  0x04c, 0x00);
			}
			break;
		case HDA_GEN_PCM_ACT_CLEANUP:

			for(i=0;i < cfg->speaker_outs;++i)
				senary_set_port_eapd(codec, cfg->speaker_pins[i], false);
			break;
	}
}
#endif

static const struct hda_codec_ops senary_auto_patch_ops = {
	.build_controls = senary_hda_gen_build_controls,
	.build_pcms = snd_hda_gen_build_pcms,
	.init = senary_auto_init,
	.free = senary_auto_free,
	.unsol_event = snd_hda_jack_unsol_event,
#ifdef CONFIG_PM
	.resume = senary_auto_resume,
	.check_power_status = snd_hda_gen_check_power_status,
#endif
};

static const struct hda_pintbl senary_pincfg_default[] = {
	{ 0x16, 0x02211020 }, /* portA, front hp */
	{ 0x17, 0x40f001f0 }, /* portG, not used inner spk 0x91170150*/
	{ 0x18, 0x05a1904d }, /* portB, top mic */
	{ 0x19, 0x02a1104e }, /* portD, front headset mic */
	{ 0x1a, 0x01819030 }, /* portC, rear linein */
	{ 0x1d, 0x01014010 }, /* portE, rear lineout 0x01114010*/
	{ 0x1f, 0x40f001f0 }, /* portH, not used */
	{ 0x24, 0x01a13040 }, /* portJ, rear mic */
	{ 0x25, 0x40f001f0 }, /* portF, not used */
	{}
};

/*
* pin fix-up
*/
enum {
	SENARY_PINCFG_DEBUG,
	SENARY_PINCFG_DEFAULT
};

static const struct hda_fixup senary_fixups[] = {
	[SENARY_PINCFG_DEFAULT] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = senary_pincfg_default,
	},
};

static const struct snd_pci_quirk sn6186_fixups[] = {
	SND_PCI_QUIRK(0x1fa8, 0xffff, "default", SENARY_PINCFG_DEFAULT),
};

static int patch_senary_auto(struct hda_codec *codec)
{
	struct senary_spec *spec;
	int err;

	codec_info(codec, "%s: BIOS auto-probing. v%d.%d.%d.%d \n", codec->core.chip_name, VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION, VERSION_BUILD);

#ifdef CONFIG_EAPD_POP
	snd_hda_codec_write(codec, 0x1b, 0,  0x04c, 0x80);
#endif

	senary_init_verb(codec);

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return -ENOMEM;
	snd_hda_gen_spec_init(&spec->gen);
	codec->spec = spec;
	codec->patch_ops = senary_auto_patch_ops;

	spec->gen.auto_mute_via_amp = 1;

	switch (codec->core.vendor_id) {
	case 0x1fa86186:
		codec->pin_amp_workaround = 1;
		spec->gen.mixer_nid = 0x15;
#ifdef DEFAULT_DEBUG
		codec_info(codec, "debug");
		snd_hda_apply_pincfgs(codec, senary_pincfg_default);
#endif
		snd_hda_pick_fixup(codec, NULL,
				sn6186_fixups, senary_fixups);
		break;
	default:
		codec->pin_amp_workaround = 1;
		snd_hda_pick_fixup(codec, NULL,
				sn6186_fixups, senary_fixups);
		break;
	}

	if (!spec->gen.vmaster_mute.hook && spec->dynamic_eapd)
		spec->gen.vmaster_mute.hook = senary_auto_vmaster_hook;

#ifdef CONFIG_FIXED_MIC_BOOST
	senary_init_fixed_mic_boost(codec);
#endif

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PRE_PROBE);

	err = snd_hda_parse_pin_defcfg(codec, &spec->gen.autocfg, NULL,
					spec->parse_flags);
	if (err < 0)
		goto error;

	err = senary_auto_parse_beep(codec);
	if (err < 0)
		goto error;

	err = snd_hda_gen_parse_auto_config(codec, &spec->gen.autocfg);
	if (err < 0)
		goto error;

	/* Some laptops with Senary chips show stalls in S3 resume,
	* which falls into the single-cmd mode.
	* Better to make reset, then.
	*/
	if (!codec->bus->core.sync_write) {
		codec_info(codec,
			"Enable sync_write for stable communication\n");
		codec->bus->core.sync_write = 1;
		codec->bus->allow_bus_reset = 1;
	}

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PROBE);
	fixup_senary_automute_switch(codec);

#ifdef CONFIG_EAPD_POP
	spec->gen.pcm_playback_hook = senary_playback_hook;
#endif
	return 0;

error:
	senary_auto_free(codec);
	return err;
}

static const struct hda_device_id snd_hda_id_senary[] = {
	HDA_CODEC_ENTRY(0x1fa86186, "SN6186", patch_senary_auto),
	{} /* terminator */
};
MODULE_DEVICE_TABLE(hdaudio, snd_hda_id_senary);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Senarytech HD-audio codec");

static struct hda_codec_driver senary_driver = {
	.id = snd_hda_id_senary,
};

module_hda_codec_driver(senary_driver);
