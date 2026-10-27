/*
 * Lop CORE - xu ly giong noi (C thuan). Xem voice_dsp.h.
 *
 * Khu vong (AEC): bo loc NLMS 256 he so hoc duong am tu loa toi micro, tru tieng vong uoc
 * luong khoi tin hieu micro. Chi cap nhat khi co tieng nguoi ben kia va KHONG phai luc ca
 * hai cung noi. Phat hien cung noi: bo loc da hoi tu -> sai so (truoc cap nhat) lon hon
 * -10 dB so voi micro; chua hoi tu -> micro manh hon tieng vong uoc luong 6 dB. Khi phat
 * hien, he so cua khoi do duoc khoi phuc (khong hoc nham giong nguoi doi mu).
 * NLP: giam them 16 dB khi chi co tieng nguoi ben kia (xoa tieng vong du).
 * Giam on: tru pho kieu Wiener, on nen uoc luong bang "minimum statistics" don gian.
 *   Bien doi: FFT thuc 512 diem cua CMSIS-DSP (arm_rfft_fast_f32) khi co CONFIG_CMSIS_DSP_TRANSFORM
 *   (hoac VDSP_CMSIS khi chay tren PC); neu khong, FFT phuc radix-2 tu viet (cham ~2-3 lan).
 * AGC: dua giong ve -20 dBFS, tang toi da +12 dB, khong tang khi chi co tieng vong.
 */

#include <math.h>
#include <string.h>

#include <me54/core/voice_dsp.h>

#if defined(CONFIG_CMSIS_DSP_TRANSFORM) || defined(VDSP_CMSIS)
#define VDSP_RFFT 1
#include <arm_math.h>
#else
#define VDSP_RFFT 0
#endif

#define PI_F 3.14159265358979f
#define L    VDSP_TAPS
#define N    VDSP_N

#define NS_WIN  320
#define NS_FFT  512
#define NS_BINS (NS_FFT / 2 + 1)

/* AEC / NLP */
#define AEC_MU     0.25f
#define AEC_DELTA  1e-4f
#define FAR_THR    1e-6f /* -60 dBFS cong suat trung binh */
#define DT_RATIO   4.0f  /* chua hoi tu: micro +6 dB so voi tieng vong uoc luong -> cung noi */
#define DT_PE_RATIO 0.1f /* da hoi tu: sai so > -10 dB so voi micro -> co giong nguoi doi mu */
#define CONVERGED_DB 6.0f
#define DT_HOLD    480   /* giu trang thai "cung noi" 30 ms */
#define NLP_GAIN   0.15f /* -16 dB */
#define NLP_SMOOTH 0.01f

/* Giam on */
#define NS_OVER  1.5f
#define NS_BIAS  2.0f    /* cuc tieu cua pho da lam muot thap hon muc on trung binh -> bu */
#define NS_FLOOR 0.18f   /* -15 dB */
#define NS_RISE  1.002f  /* on nen duoc phep tang ~0.9 dB/s */

/* AGC */
#define AGC_TARGET 0.1f   /* -20 dBFS */
#define AGC_MAX    4.0f   /* +12 dB */
#define AGC_MIN    0.25f
#define AGC_GATE   0.003f /* -50 dBFS */

struct biquad {
	float x1, x2, y1, y2;
};

static bool en_aec = true;
static bool en_ns = true;
static bool en_agc = true;

/* Loc thong cao 100 Hz (cung he so cho micro va tham chieu) */
static float hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;
static struct biquad hp_mic;
static struct biquad hp_ref;

/* AEC */
static float wr[L];          /* he so dao nguoc: wr[i] nhan voi xb[n + i] */
static float wr_backup[L];   /* he so truoc khoi hien tai (khoi phuc khi phat hien cung noi) */
static float xb[L - 1 + N];  /* lich su tham chieu: L-1 mau cu + N mau moi */
static float echo_gain;      /* uoc luong Pd / Px khi chi co nguoi ben kia */
static int dt_hold;
static float nlp_g;
static float erle_db;

/* Giam on */
static float win[NS_WIN];
#if VDSP_RFFT
static arm_rfft_fast_instance_f32 rfft;
static float ns_time[NS_FFT]; /* khung mien thoi gian (arm_rfft_fast_f32 ghi de dau vao) */
static float ns_spec[NS_FFT]; /* pho dong goi: [0] DC, [1] Nyquist, [2k] + j[2k+1] = bin k */
#else
static float tw_re[NS_FFT / 2];
static float tw_im[NS_FFT / 2];
static float fre[NS_FFT];
static float fim[NS_FFT];
#endif
static float ns_in[NS_WIN];
static float ns_ola[N];
static float psd_s[NS_BINS];
static float psd_n[NS_BINS];
static float gain_s[NS_BINS];
static bool ns_primed;

/* AGC */
static float agc_g;

static struct vdsp_stats stats;

static float biquad_run(struct biquad *q, float x)
{
	const float y = hp_b0 * x + hp_b1 * q->x1 + hp_b2 * q->x2 - hp_a1 * q->y1 - hp_a2 * q->y2;

	q->x2 = q->x1;
	q->x1 = x;
	q->y2 = q->y1;
	q->y1 = y;
	return y;
}

static float mean_sq(const float *x, int n)
{
	float s = 0.0f;

	for (int i = 0; i < n; i++) {
		s += x[i] * x[i];
	}
	return s / (float)n;
}

static float to_db(float p)
{
	return 10.0f * log10f(p + 1e-12f);
}

#if !VDSP_RFFT
/* FFT phuc radix-2 tai cho, NS_FFT diem */
static void fft(float *re, float *im, bool inverse)
{
	for (int i = 1, j = 0; i < NS_FFT; i++) {
		int bit = NS_FFT >> 1;

		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float t = re[i];

			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}

	for (int len = 2; len <= NS_FFT; len <<= 1) {
		const int half = len >> 1;
		const int step = NS_FFT / len;

		for (int i = 0; i < NS_FFT; i += len) {
			for (int k = 0; k < half; k++) {
				const float wre = tw_re[k * step];
				const float wim = inverse ? -tw_im[k * step] : tw_im[k * step];
				const int a = i + k;
				const int b = a + half;
				const float xr = re[b] * wre - im[b] * wim;
				const float xi = re[b] * wim + im[b] * wre;

				re[b] = re[a] - xr;
				im[b] = im[a] - xi;
				re[a] += xr;
				im[a] += xi;
			}
		}
	}

	if (inverse) {
		const float s = 1.0f / NS_FFT;

		for (int i = 0; i < NS_FFT; i++) {
			re[i] *= s;
			im[i] *= s;
		}
	}
}
#endif /* !VDSP_RFFT */

void vdsp_init(void)
{
	/* RBJ high-pass, fc = 100 Hz, Q = 0.7071 */
	const float w0 = 2.0f * PI_F * 100.0f / VDSP_FS;
	const float alpha = sinf(w0) / (2.0f * 0.70710678f);
	const float cw = cosf(w0);
	const float a0 = 1.0f + alpha;

	hp_b0 = (1.0f + cw) / 2.0f / a0;
	hp_b1 = -(1.0f + cw) / a0;
	hp_b2 = hp_b0;
	hp_a1 = -2.0f * cw / a0;
	hp_a2 = (1.0f - alpha) / a0;

	/* Cua so sqrt-Hann (chu ky): win^2 chong 50 % cong lai = 1 -> tai tao hoan hao */
	for (int i = 0; i < NS_WIN; i++) {
		win[i] = sqrtf(0.5f - 0.5f * cosf(2.0f * PI_F * i / NS_WIN));
	}
#if VDSP_RFFT
	(void)arm_rfft_fast_init_512_f32(&rfft); /* chi keo bang cua co 512 vao flash */
#else
	for (int k = 0; k < NS_FFT / 2; k++) {
		tw_re[k] = cosf(2.0f * PI_F * k / NS_FFT);
		tw_im[k] = -sinf(2.0f * PI_F * k / NS_FFT);
	}
#endif

	vdsp_reset();
}

void vdsp_aec_reset(void)
{
	memset(wr, 0, sizeof(wr));
	memset(xb, 0, sizeof(xb));
	echo_gain = 1.0f;
	dt_hold = 0;
	nlp_g = 1.0f;
	erle_db = 0.0f;
}

void vdsp_reset(void)
{
	memset(&hp_mic, 0, sizeof(hp_mic));
	memset(&hp_ref, 0, sizeof(hp_ref));
	vdsp_aec_reset();
	memset(ns_in, 0, sizeof(ns_in));
	memset(ns_ola, 0, sizeof(ns_ola));
	for (int k = 0; k < NS_BINS; k++) {
		gain_s[k] = 1.0f;
	}
	ns_primed = false;
	agc_g = 1.0f;
	memset(&stats, 0, sizeof(stats));
}

void vdsp_set(bool aec, bool ns, bool agc)
{
	if (aec && !en_aec) {
		vdsp_aec_reset();
	}
	en_aec = aec;
	en_ns = ns;
	en_agc = agc;
}

void vdsp_get_flags(bool *aec, bool *ns, bool *agc)
{
	*aec = en_aec;
	*ns = en_ns;
	*agc = en_agc;
}

/* NLMS tren 1 khoi: d (micro) -> d (sai so = micro - tieng vong uoc luong) */
static void aec_block(const float *ref, float *d, bool adapt)
{
	float energy = 0.0f;

	memcpy(&xb[L - 1], ref, N * sizeof(float));
	for (int i = 0; i < L; i++) {
		energy += xb[i] * xb[i];
	}

	for (int n = 0; n < N; n++) {
		const float *x = &xb[n];
		float y = 0.0f;
		float e;

		for (int i = 0; i < L; i++) {
			y += wr[i] * x[i];
		}
		e = d[n] - y;

		if (adapt) {
			const float g = AEC_MU * e / (energy + AEC_DELTA);

			for (int i = 0; i < L; i++) {
				wr[i] += g * x[i];
			}
		}
		d[n] = e;

		if (n + 1 < N) {
			energy += x[L] * x[L] - x[0] * x[0];
			if (energy < 0.0f) {
				energy = 0.0f;
			}
		}
	}

	memmove(xb, &xb[N], (L - 1) * sizeof(float));
}

/* Cap nhat pho on nen va tinh he so khuech dai cho bin k (p = cong suat cua bin) */
static float ns_bin_gain(int k, float p)
{
	float g;

	psd_s[k] = ns_primed ? 0.8f * psd_s[k] + 0.2f * p : p;
	if (!ns_primed || psd_s[k] < psd_n[k]) {
		psd_n[k] = psd_s[k];
	} else {
		psd_n[k] *= NS_RISE;
	}

	g = 1.0f - NS_OVER * NS_BIAS * psd_n[k] / (psd_s[k] + 1e-12f);
	g = g < NS_FLOOR ? NS_FLOOR : (g > 1.0f ? 1.0f : g);
	gain_s[k] = 0.6f * gain_s[k] + 0.4f * g;
	return gain_s[k];
}

/* Giam on pho, cua so 20 ms, buoc 10 ms (tre them 10 ms) */
static void ns_block(float *x)
{
	const float *y;

	memmove(ns_in, &ns_in[N], (NS_WIN - N) * sizeof(float));
	memcpy(&ns_in[NS_WIN - N], x, N * sizeof(float));

#if VDSP_RFFT
	for (int i = 0; i < NS_FFT; i++) {
		ns_time[i] = (i < NS_WIN) ? ns_in[i] * win[i] : 0.0f;
	}
	arm_rfft_fast_f32(&rfft, ns_time, ns_spec, 0);

	/* DC va Nyquist la so thuc */
	ns_spec[0] *= ns_bin_gain(0, ns_spec[0] * ns_spec[0]);
	ns_spec[1] *= ns_bin_gain(NS_FFT / 2, ns_spec[1] * ns_spec[1]);
	for (int k = 1; k < NS_FFT / 2; k++) {
		float *b = &ns_spec[2 * k];
		const float g = ns_bin_gain(k, b[0] * b[0] + b[1] * b[1]);

		b[0] *= g;
		b[1] *= g;
	}
	ns_primed = true;

	arm_rfft_fast_f32(&rfft, ns_spec, ns_time, 1);
	y = ns_time;
#else
	for (int i = 0; i < NS_FFT; i++) {
		fre[i] = (i < NS_WIN) ? ns_in[i] * win[i] : 0.0f;
		fim[i] = 0.0f;
	}
	fft(fre, fim, false);

	for (int k = 0; k < NS_BINS; k++) {
		const float g = ns_bin_gain(k, fre[k] * fre[k] + fim[k] * fim[k]);

		fre[k] *= g;
		fim[k] *= g;
		if (k > 0 && k < NS_FFT / 2) {
			fre[NS_FFT - k] *= g;
			fim[NS_FFT - k] *= g;
		}
	}
	ns_primed = true;

	fft(fre, fim, true);
	y = fre;
#endif

	for (int n = 0; n < N; n++) {
		x[n] = ns_ola[n] + y[n] * win[n];
		ns_ola[n] = y[N + n] * win[N + n];
	}
}

static void agc_block(float *x, bool echo_only)
{
	const float rms = sqrtf(mean_sq(x, N));
	float g_new = agc_g;

	if (rms > AGC_GATE && !echo_only) {
		float desired = AGC_TARGET / rms;

		desired = desired < AGC_MIN ? AGC_MIN : (desired > AGC_MAX ? AGC_MAX : desired);
		/* giam nhanh (tranh vo tieng), tang cham */
		g_new = desired < agc_g ? 0.5f * agc_g + 0.5f * desired
					: 0.98f * agc_g + 0.02f * desired;
	}

	for (int n = 0; n < N; n++) {
		const float g = agc_g + (g_new - agc_g) * (float)(n + 1) / (float)N;
		float v = x[n] * g;

		x[n] = v > 0.98f ? 0.98f : (v < -0.98f ? -0.98f : v);
	}
	agc_g = g_new;
}

void vdsp_process(const int16_t *mic_lr, const int16_t *ref, int16_t *out)
{
	float d[N];
	float r[N];
	float pd;
	float px;
	float pe;
	bool far;
	bool dt;

	for (int n = 0; n < N; n++) {
		const float m = ((float)mic_lr[2 * n] + (float)mic_lr[2 * n + 1]) * (0.5f / 32768.0f);
		const float x = ref ? (float)ref[n] * (1.0f / 32768.0f) : 0.0f;

		d[n] = biquad_run(&hp_mic, m);
		r[n] = biquad_run(&hp_ref, x);
	}

	pd = mean_sq(d, N);
	px = mean_sq(r, N);
	far = ref != NULL && px > FAR_THR;

	/* Uoc luong muc tieng vong (Pd/Px) theo cuc tieu, dung de phat hien ca hai cung noi */
	if (far) {
		const float ratio = pd / (px + 1e-12f);

		echo_gain = ratio < echo_gain ? 0.7f * echo_gain + 0.3f * ratio : echo_gain * 1.01f;
		echo_gain = echo_gain < 1e-4f ? 1e-4f : (echo_gain > 1e4f ? 1e4f : echo_gain);
	}
	if (far) {
		stats.far_blocks++;
	}

	/* Bo loc van nhan tham chieu ca khi tat AEC de bat lai la dung ngay */
	if (en_aec) {
		const bool adapt = far && dt_hold <= 0;

		if (adapt) {
			memcpy(wr_backup, wr, sizeof(wr));
		}
		aec_block(r, d, adapt);
		pe = mean_sq(d, N);

		dt = far && (erle_db > CONVERGED_DB ? pe > DT_PE_RATIO * pd
						    : pd > DT_RATIO * echo_gain * px);
		if (dt && adapt) {
			memcpy(wr, wr_backup, sizeof(wr)); /* bo phan hoc nham trong khoi nay */
		}
	} else {
		memcpy(&xb[L - 1], r, N * sizeof(float));
		memmove(xb, &xb[N], (L - 1) * sizeof(float));
		pe = mean_sq(d, N);
		dt = far && pd > DT_RATIO * echo_gain * px;
	}

	if (dt) {
		dt_hold = DT_HOLD;
		stats.dt_blocks++;
	} else if (dt_hold > 0) {
		dt_hold -= N;
	}

	if (en_aec && far && pe > 4.0f * pd) {
		/* Bo loc phan ky (lam tieng to hon) -> lam lai tu dau */
		memset(wr, 0, sizeof(wr));
		stats.resets++;
	}
	if (en_aec && far && dt_hold <= 0 && pe > 0.0f) {
		erle_db = 0.9f * erle_db + 0.1f * (to_db(pd) - to_db(pe));
	}

	/* NLP: chi co tieng nguoi ben kia -> giam manh phan con lai */
	{
		const float target = (far && dt_hold <= 0) ? NLP_GAIN : 1.0f;

		for (int n = 0; n < N; n++) {
			nlp_g += (target - nlp_g) * NLP_SMOOTH;
			d[n] *= nlp_g;
		}
	}

	if (en_ns) {
		ns_block(d);
	}
	if (en_agc) {
		agc_block(d, far && dt_hold <= 0);
	}

	for (int n = 0; n < N; n++) {
		float v = d[n] * 32768.0f;

		v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
		out[n] = (int16_t)v;
	}

	stats.mic_dbfs = to_db(pd);
	stats.out_dbfs = to_db(mean_sq(d, N));
	stats.erle_db = erle_db;
}

void vdsp_get_stats(struct vdsp_stats *st)
{
	*st = stats;
}
