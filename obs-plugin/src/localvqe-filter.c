#include <obs-module.h>
#include <media-io/audio-resampler.h>
#include <util/deque.h>
#include <util/threading.h>
#include <util/platform.h>

#include <localvqe_api.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define LVQE_RATE      16000u
#define LVQE_HOP       256u
#define LVQE_HOP_BYTES (LVQE_HOP * sizeof(float))

/* Sanity: anywhere we drain the hop loop, cap iterations so a bad timestamp
 * or queue-size invariant can never wedge filter_audio. At 16 kHz a single
 * OBS callback delivers at most ~10–20 ms of audio (one or two hops), so
 * many hundreds of hops in a single call already means something is wrong. */
#define LVQE_MAX_HOPS_PER_CALL 1024u

/* The downsampler can't legitimately produce more frames than the upper
 * bound below (LVQE_RATE ≤ obs_rate in practice, but cap loosely either
 * way). Used to sanity-check audio_resampler_resample outputs. */
#define LVQE_MAX_FRAMES_PER_CHUNK 65536u

#define LVQE_BLOG(level, fmt, ...) \
	blog(level, "[obs-localvqe] " fmt, ##__VA_ARGS__)

_Static_assert(LVQE_HOP_BYTES == LVQE_HOP * sizeof(float),
	       "hop byte count must equal LVQE_HOP * sizeof(float)");
_Static_assert(LVQE_HOP > 0 && LVQE_HOP <= 4096,
	       "LVQE_HOP must be a reasonable positive frame count");
_Static_assert(LVQE_RATE > 0, "LVQE_RATE must be positive");

#define S_MODEL_PATH       "model_path"
#define S_NOISE_GATE_EN    "noise_gate_enabled"
#define S_NOISE_GATE_DB    "noise_gate_threshold_dbfs"
#define S_REF_SOURCE       "reference_source"

#define T_(s)              obs_module_text(s)

struct lvqe_filter {
	obs_source_t *parent;

	localvqe_ctx_t ctx;
	char *model_path;

	uint32_t obs_rate;       /* OBS mix rate, typically 48000 */
	uint32_t obs_channels;   /* OBS mix channels */

	/* OBS rate × N planar  →  16 kHz mono float */
	audio_resampler_t *down;
	/* 16 kHz mono float    →  OBS rate × N planar */
	audio_resampler_t *up;

	/* 16 kHz mono float ring buffers (bytes). */
	struct deque in_16k;
	struct deque out_16k;
	uint64_t     in_16k_head_ts;   /* timestamp of in_16k[0], if has_ts */
	bool         in_16k_has_ts;

	float hop_mic[LVQE_HOP];
	float hop_ref[LVQE_HOP];
	float hop_out[LVQE_HOP];

	/* Sidechain reference (AEC). NULL → zeros into the ref input. */
	obs_weak_source_t *weak_ref;
	pthread_mutex_t ref_lock;
	struct deque ref_16k;
	uint64_t     ref_16k_head_ts;
	bool         ref_16k_has_ts;
	audio_resampler_t *ref_down;
	uint32_t ref_rate;
	uint32_t ref_channels;

	/* Reported algorithmic latency: one hop @ 16 kHz, in ns. */
	uint64_t latency_ns;
};

/* ── Helpers ────────────────────────────────────────────────────────── */

static inline uint64_t samples_to_ns(uint64_t n)
{
	return (n * 1000000000ull) / LVQE_RATE;
}

/* deque_pop_front does memcpy(dst, ...) — it doesn't accept NULL. Discard
 * stale bytes in chunks so we don't allocate. */
static void deque_drop_front(struct deque *q, size_t bytes)
{
	assert(q != NULL);
	assert(bytes <= q->size);
	uint8_t scratch[1024];
	while (bytes > 0) {
		size_t n = bytes < sizeof(scratch) ? bytes : sizeof(scratch);
		deque_pop_front(q, scratch, n);
		bytes -= n;
	}
}


static void release_resamplers(struct lvqe_filter *f)
{
	if (f->down) { audio_resampler_destroy(f->down); f->down = NULL; }
	if (f->up)   { audio_resampler_destroy(f->up);   f->up   = NULL; }
	if (f->ref_down) { audio_resampler_destroy(f->ref_down); f->ref_down = NULL; }
}

static void build_io_resamplers(struct lvqe_filter *f)
{
	assert(f != NULL);
	assert(f->obs_rate > 0 && f->obs_rate <= 384000);
	assert(f->obs_channels >= 1 && f->obs_channels <= MAX_AV_PLANES);

	struct resample_info obs_side = {
		.samples_per_sec = f->obs_rate,
		.format          = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers        = (enum speaker_layout)f->obs_channels,
	};
	struct resample_info lvqe_side = {
		.samples_per_sec = LVQE_RATE,
		.format          = AUDIO_FORMAT_FLOAT,
		.speakers        = SPEAKERS_MONO,
	};
	f->down = audio_resampler_create(&lvqe_side, &obs_side);
	f->up   = audio_resampler_create(&obs_side, &lvqe_side);
	if (!f->down || !f->up) {
		LVQE_BLOG(LOG_ERROR,
			  "failed to build resamplers (rate=%u ch=%u)",
			  f->obs_rate, f->obs_channels);
	}
}

static void detach_reference(struct lvqe_filter *f);

/* ── Sidechain (reference / AEC) ────────────────────────────────────── */

static void ref_audio_cb(void *param, obs_source_t *src,
			 const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(src);
	struct lvqe_filter *f = param;
	assert(f != NULL);
	if (!audio || !audio->frames)
		return;
	if (audio->frames > LVQE_MAX_FRAMES_PER_CHUNK) {
		LVQE_BLOG(LOG_WARNING,
			  "ref chunk implausibly large (%u frames); dropping",
			  audio->frames);
		return;
	}
	if (!muted && !audio->data[0]) {
		/* OBS contract: data[0] is non-NULL for any audible chunk. */
		return;
	}

	pthread_mutex_lock(&f->ref_lock);

	/* Lazily build / rebuild the reference resampler if the source's
	 * native rate/channel count changed since last callback. */
	if (!f->ref_down || f->ref_rate != f->obs_rate /* assume mixed at obs rate */) {
		if (f->ref_down) audio_resampler_destroy(f->ref_down);
		struct resample_info ref_side = {
			.samples_per_sec = f->obs_rate,
			.format          = AUDIO_FORMAT_FLOAT_PLANAR,
			.speakers        = (enum speaker_layout)f->obs_channels,
		};
		struct resample_info lvqe_side = {
			.samples_per_sec = LVQE_RATE,
			.format          = AUDIO_FORMAT_FLOAT,
			.speakers        = SPEAKERS_MONO,
		};
		f->ref_down    = audio_resampler_create(&lvqe_side, &ref_side);
		f->ref_rate    = f->obs_rate;
		f->ref_channels = f->obs_channels;
	}

	uint8_t *out[MAX_AV_PLANES] = {0};
	uint32_t out_frames = 0;
	uint64_t ts_off = 0;
	const uint8_t *in[MAX_AV_PLANES];
	for (size_t i = 0; i < MAX_AV_PLANES; ++i)
		in[i] = audio->data[i];

	bool was_empty = (f->ref_16k.size == 0);

	if (muted) {
		uint32_t silent = (audio->frames * LVQE_RATE) / f->obs_rate;
		assert(silent <= LVQE_MAX_FRAMES_PER_CHUNK);
		float zero = 0.0f;
		for (uint32_t i = 0; i < silent; ++i)
			deque_push_back(&f->ref_16k, &zero, sizeof(float));
		out_frames = silent;
	} else if (!f->ref_down) {
		out_frames = 0;
	} else if (!audio_resampler_resample(f->ref_down, out, &out_frames,
					    &ts_off, in, audio->frames)) {
		out_frames = 0;
	} else if (out_frames > LVQE_MAX_FRAMES_PER_CHUNK || !out[0]) {
		LVQE_BLOG(LOG_WARNING,
			  "ref resampler returned implausible output "
			  "(frames=%u, out[0]=%p); dropping",
			  out_frames, (void *)out[0]);
		out_frames = 0;
	} else {
		deque_push_back(&f->ref_16k, out[0],
				out_frames * sizeof(float));
	}

	if (was_empty && out_frames > 0) {
		/* Resampler ts_offset is small (sub-ms) and identical between
		 * the mic and ref resamplers, so it cancels in alignment math.
		 * Use the input chunk timestamp directly. */
		f->ref_16k_head_ts = audio->timestamp;
		f->ref_16k_has_ts = true;
	}

	pthread_mutex_unlock(&f->ref_lock);
}

static void detach_reference(struct lvqe_filter *f)
{
	if (f->weak_ref) {
		obs_source_t *src = obs_weak_source_get_source(f->weak_ref);
		if (src) {
			obs_source_remove_audio_capture_callback(
				src, ref_audio_cb, f);
			obs_source_release(src);
		}
		obs_weak_source_release(f->weak_ref);
		f->weak_ref = NULL;
	}
	pthread_mutex_lock(&f->ref_lock);
	deque_free(&f->ref_16k);
	f->ref_16k_has_ts = false;
	pthread_mutex_unlock(&f->ref_lock);
}

static void attach_reference(struct lvqe_filter *f, const char *name)
{
	assert(f != NULL);
	detach_reference(f);
	if (!name || !*name) return;

	obs_source_t *src = obs_get_source_by_name(name);
	if (!src) {
		LVQE_BLOG(LOG_WARNING,
			  "reference source '%s' not found", name);
		return;
	}
	f->weak_ref = obs_source_get_weak_source(src);
	if (!f->weak_ref) {
		LVQE_BLOG(LOG_WARNING,
			  "failed to weak-ref reference source '%s'", name);
		obs_source_release(src);
		return;
	}
	obs_source_add_audio_capture_callback(src, ref_audio_cb, f);
	obs_source_release(src);
}

/* ── obs_source_info callbacks ──────────────────────────────────────── */

static const char *lvqe_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("LocalVQE.FilterName");
}

static void lvqe_update(void *data, obs_data_t *settings)
{
	struct lvqe_filter *f = data;
	assert(f != NULL);
	assert(settings != NULL);

	const char *path = obs_data_get_string(settings, S_MODEL_PATH);
	if (!path) path = "";

	/* (Re)load the model when the path changes. */
	if (!f->model_path || strcmp(f->model_path, path) != 0) {
		if (f->ctx) { localvqe_free(f->ctx); f->ctx = 0; }
		bfree(f->model_path);
		f->model_path = bstrdup(path);
		if (*path) {
			f->ctx = localvqe_new(path);
			if (!f->ctx) {
				LVQE_BLOG(LOG_ERROR,
					  "localvqe_new failed for '%s'", path);
			} else {
				/* Contract check: confirm the model agrees with
				 * our compile-time framing assumptions. */
				int sr = localvqe_sample_rate(f->ctx);
				int hop = localvqe_hop_length(f->ctx);
				if (sr != (int)LVQE_RATE || hop != (int)LVQE_HOP) {
					LVQE_BLOG(LOG_ERROR,
						  "model framing mismatch: "
						  "sr=%d hop=%d (expected %u/%u)",
						  sr, hop, LVQE_RATE, LVQE_HOP);
					localvqe_free(f->ctx);
					f->ctx = 0;
				}
			}
		}
	}

	if (f->ctx) {
		int en = (int)obs_data_get_bool(settings, S_NOISE_GATE_EN);
		double db = obs_data_get_double(settings, S_NOISE_GATE_DB);
		/* Clamp to the same dBFS range the property slider exposes —
		 * keeps anything pathological out of the model. */
		if (db < -120.0) db = -120.0;
		if (db >    0.0) db =    0.0;
		localvqe_set_noise_gate(f->ctx, en, (float)db);
	}

	const char *ref_name = obs_data_get_string(settings, S_REF_SOURCE);
	attach_reference(f, ref_name ? ref_name : "");
}

static void *lvqe_create(obs_data_t *settings, obs_source_t *source)
{
	assert(settings != NULL);
	assert(source != NULL);

	struct lvqe_filter *f = bzalloc(sizeof(*f));
	assert(f != NULL); /* bzalloc aborts on OOM, but document intent */
	f->parent = source;
	pthread_mutex_init(&f->ref_lock, NULL);

	audio_t *a = obs_get_audio();
	if (!a) {
		LVQE_BLOG(LOG_ERROR, "obs_get_audio() returned NULL");
		bfree(f);
		return NULL;
	}
	f->obs_rate     = audio_output_get_sample_rate(a);
	f->obs_channels = audio_output_get_channels(a);

	if (f->obs_rate == 0 || f->obs_channels == 0 ||
	    f->obs_channels > MAX_AV_PLANES) {
		LVQE_BLOG(LOG_ERROR,
			  "implausible OBS audio config: rate=%u ch=%u",
			  f->obs_rate, f->obs_channels);
		pthread_mutex_destroy(&f->ref_lock);
		bfree(f);
		return NULL;
	}
	/* OBS's filter_audio always delivers planar float at the mix; warn
	 * if the mix is configured otherwise — we'd silently mis-interpret
	 * the data otherwise. */
	if (audio_output_get_format(a) != AUDIO_FORMAT_FLOAT_PLANAR) {
		LVQE_BLOG(LOG_WARNING,
			  "OBS audio format is not FLOAT_PLANAR (%d) — "
			  "filter will pass audio through",
			  (int)audio_output_get_format(a));
	}

	build_io_resamplers(f);

	/* one 16 ms hop @ 16 kHz */
	f->latency_ns = samples_to_ns(LVQE_HOP);

	lvqe_update(f, settings);
	return f;
}

static void lvqe_destroy(void *data)
{
	struct lvqe_filter *f = data;
	assert(f != NULL);
	detach_reference(f);
	release_resamplers(f);
	if (f->ctx) localvqe_free(f->ctx);
	bfree(f->model_path);
	deque_free(&f->in_16k);
	deque_free(&f->out_16k);
	deque_free(&f->ref_16k);
	pthread_mutex_destroy(&f->ref_lock);
	bfree(f);
}

static void lvqe_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, S_MODEL_PATH, "");
	obs_data_set_default_bool(s, S_NOISE_GATE_EN, false);
	obs_data_set_default_double(s, S_NOISE_GATE_DB, -45.0);
	obs_data_set_default_string(s, S_REF_SOURCE, "");
}

static bool enum_audio_sources(void *param, obs_source_t *src)
{
	obs_property_t *p = param;
	assert(p != NULL);
	if (!src) return true;
	uint32_t flags = obs_source_get_output_flags(src);
	if ((flags & OBS_SOURCE_AUDIO) == 0) return true;
	const char *name = obs_source_get_name(src);
	if (!name || !*name) return true;
	obs_property_list_add_string(p, name, name);
	return true;
}

static obs_properties_t *lvqe_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, S_MODEL_PATH, T_("LocalVQE.ModelPath"),
				OBS_PATH_FILE, "GGUF (*.gguf)", NULL);

	obs_properties_add_bool(props, S_NOISE_GATE_EN,
				T_("LocalVQE.NoiseGate.Enabled"));
	obs_properties_add_float_slider(props, S_NOISE_GATE_DB,
					T_("LocalVQE.NoiseGate.ThresholdDbFS"),
					-90.0, -10.0, 0.5);

	obs_property_t *ref = obs_properties_add_list(
		props, S_REF_SOURCE, T_("LocalVQE.ReferenceSource"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(ref, T_("LocalVQE.ReferenceSource.None"), "");
	obs_enum_sources(enum_audio_sources, ref);

	return props;
}

/* ── filter_audio: the hot path ─────────────────────────────────────── */

static struct obs_audio_data *lvqe_filter_audio(void *data,
						struct obs_audio_data *audio)
{
	struct lvqe_filter *f = data;
	assert(f != NULL);
	if (!audio) return NULL;
	if (audio->frames == 0) return audio;
	if (audio->frames > LVQE_MAX_FRAMES_PER_CHUNK) {
		LVQE_BLOG(LOG_WARNING,
			  "mic chunk implausibly large (%u frames); "
			  "passing through unmodified",
			  audio->frames);
		return audio;
	}
	if (!audio->data[0]) {
		/* OBS contract violation — first plane must exist. */
		return audio;
	}
	if (!f->ctx || !f->down || !f->up)
		return audio; /* not configured — pass through */

	assert(f->obs_channels >= 1 && f->obs_channels <= MAX_AV_PLANES);
	assert(f->in_16k.size  % sizeof(float) == 0);
	assert(f->out_16k.size % sizeof(float) == 0);

	/* 1. Downsample the mic chunk to 16 kHz mono → in_16k. */
	{
		uint8_t *out[MAX_AV_PLANES] = {0};
		uint32_t out_frames = 0;
		uint64_t ts_off = 0;
		const uint8_t *in[MAX_AV_PLANES];
		for (size_t i = 0; i < MAX_AV_PLANES; ++i)
			in[i] = audio->data[i];

		bool was_empty = (f->in_16k.size == 0);
		if (audio_resampler_resample(f->down, out, &out_frames,
					     &ts_off, in, audio->frames)) {
			if (out_frames > LVQE_MAX_FRAMES_PER_CHUNK || !out[0]) {
				LVQE_BLOG(LOG_WARNING,
					  "mic resampler returned implausible "
					  "output (frames=%u, out[0]=%p)",
					  out_frames, (void *)out[0]);
			} else if (out_frames > 0) {
				deque_push_back(&f->in_16k, out[0],
						out_frames * sizeof(float));
				if (was_empty) {
					f->in_16k_head_ts = audio->timestamp;
					f->in_16k_has_ts = true;
				}
			}
		}
	}

	/* 2. Drain as many 256-frame hops as we can. */
	const uint64_t hop_ns = samples_to_ns(LVQE_HOP);
	uint32_t hops_done = 0;
	while (f->in_16k.size >= LVQE_HOP_BYTES &&
	       hops_done < LVQE_MAX_HOPS_PER_CALL) {
		++hops_done;
		assert(f->in_16k_has_ts);
		assert(f->in_16k.size % sizeof(float) == 0);

		uint64_t mic_hop_ts = f->in_16k_head_ts;
		deque_pop_front(&f->in_16k, f->hop_mic, LVQE_HOP_BYTES);
		f->in_16k_head_ts += hop_ns;
		if (f->in_16k.size == 0) f->in_16k_has_ts = false;

		/* Align ref to this mic hop's timestamp. AlignBlock soaks
		 * the residual ≤320 ms; we only need to keep the queues
		 * within that window. Lag = how far ref lags mic. */
		bool used_ref = false;
		pthread_mutex_lock(&f->ref_lock);
		if (f->ref_16k_has_ts) {
			int64_t lag_ns = (int64_t)mic_hop_ts -
					 (int64_t)f->ref_16k_head_ts;
			if (lag_ns > 0) {
				/* Ref is older than mic — drop the stale prefix. */
				uint64_t drop_samples =
					((uint64_t)lag_ns * LVQE_RATE) /
					1000000000ull;
				size_t drop_bytes = drop_samples * sizeof(float);
				if (drop_bytes > f->ref_16k.size)
					drop_bytes = f->ref_16k.size;
				if (drop_bytes) {
					deque_drop_front(&f->ref_16k, drop_bytes);
					f->ref_16k_head_ts += samples_to_ns(
						drop_bytes / sizeof(float));
				}
			}
			/* If ref is now in the past or roughly aligned (head_ts
			 * within one hop of mic) and we have a full hop, use it.
			 * Otherwise the ref is still in the future — feed zeros
			 * for this hop and let it accumulate. */
			int64_t lead_ns = (int64_t)f->ref_16k_head_ts -
					  (int64_t)mic_hop_ts;
			if (lead_ns < (int64_t)hop_ns &&
			    f->ref_16k.size >= LVQE_HOP_BYTES) {
				deque_pop_front(&f->ref_16k, f->hop_ref,
						LVQE_HOP_BYTES);
				f->ref_16k_head_ts += hop_ns;
				if (f->ref_16k.size == 0)
					f->ref_16k_has_ts = false;
				used_ref = true;
			}
		}
		pthread_mutex_unlock(&f->ref_lock);

		/* No reference (no sidechain configured, or it hasn't caught
		 * up yet) — feed zeros so the AEC head sees "nothing playing".
		 * Never feed mic here; mic == ref would cancel everything. */
		if (!used_ref) memset(f->hop_ref, 0, LVQE_HOP_BYTES);

		assert(f->hop_mic && f->hop_ref && f->hop_out);
		if (localvqe_process_frame_f32(f->ctx, f->hop_mic, f->hop_ref,
					       LVQE_HOP, f->hop_out) != 0) {
			LVQE_BLOG(LOG_WARNING,
				  "localvqe_process_frame_f32 failed: %s",
				  localvqe_last_error(f->ctx));
			memcpy(f->hop_out, f->hop_mic, LVQE_HOP_BYTES);
		}

		deque_push_back(&f->out_16k, f->hop_out, LVQE_HOP_BYTES);
	}
	if (hops_done >= LVQE_MAX_HOPS_PER_CALL) {
		LVQE_BLOG(LOG_WARNING,
			  "hit hop drain cap (%u) in one filter_audio call — "
			  "queues may have desynced",
			  LVQE_MAX_HOPS_PER_CALL);
	}

	/* 3. Decide how many 16 kHz frames to upsample so the output chunk
	 *    matches the input chunk in OBS-rate frames. Hold output back
	 *    until we have at least that much; otherwise drop this callback
	 *    (OBS sees us as buffering, reflected in latency_ns). */
	uint32_t want_16k =
		(uint32_t)(((uint64_t)audio->frames * LVQE_RATE) / f->obs_rate);
	assert(want_16k <= LVQE_MAX_FRAMES_PER_CHUNK);
	if (want_16k == 0) return audio;
	if ((uint64_t)f->out_16k.size < (uint64_t)want_16k * sizeof(float))
		return NULL;

	float *scratch = bmalloc(want_16k * sizeof(float));
	assert(scratch != NULL);
	deque_pop_front(&f->out_16k, scratch, want_16k * sizeof(float));

	const uint8_t *in_planes[MAX_AV_PLANES] = { (uint8_t *)scratch };
	uint8_t *out_planes[MAX_AV_PLANES] = {0};
	uint32_t out_frames = 0;
	uint64_t ts_off = 0;
	bool ok = audio_resampler_resample(f->up, out_planes, &out_frames,
					   &ts_off, in_planes, want_16k);
	bfree(scratch);
	if (!ok) return NULL;
	if (out_frames == 0 || out_frames > LVQE_MAX_FRAMES_PER_CHUNK) {
		LVQE_BLOG(LOG_WARNING,
			  "up-resampler returned implausible frame count (%u)",
			  out_frames);
		return NULL;
	}

	/* The resampler-owned planes live until the next call; copy them
	 * back onto the obs_audio_data buffer that the caller will keep
	 * reading from. */
	uint32_t copy_frames = out_frames < audio->frames ? out_frames
							  : audio->frames;
	size_t bytes = (size_t)copy_frames * sizeof(float);
	for (size_t ch = 0; ch < f->obs_channels && ch < MAX_AV_PLANES; ++ch) {
		if (audio->data[ch] && out_planes[ch])
			memcpy(audio->data[ch], out_planes[ch], bytes);
	}

	/* Don't let timestamp underflow into garbage if OBS hands us a
	 * very early chunk (e.g. start-of-stream). */
	if (audio->timestamp >= f->latency_ns)
		audio->timestamp -= f->latency_ns;
	else
		audio->timestamp = 0;
	return audio;
}

struct obs_source_info localvqe_filter_info = {
	.id             = "localvqe_filter",
	.type           = OBS_SOURCE_TYPE_FILTER,
	.output_flags   = OBS_SOURCE_AUDIO,
	.get_name       = lvqe_get_name,
	.create         = lvqe_create,
	.destroy        = lvqe_destroy,
	.update         = lvqe_update,
	.get_defaults   = lvqe_defaults,
	.get_properties = lvqe_properties,
	.filter_audio   = lvqe_filter_audio,
};
