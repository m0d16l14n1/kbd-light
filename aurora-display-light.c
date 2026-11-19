// SPDX-License-Identifier: MIT
/*
 * aurora-display-light: drive the built-in display's brightness from the
 * ambient light sensor, the way macOS does.
 *
 * The model is macOS's own (CoreBrightness, CBAutoBrightnessModuleSKL):
 *
 * - The light is mapped to a brightness in nits along a straight line,
 *   NITS(lux) = L1 + (L2 - L1) * lux / E2, with lux capped at 5000. The
 *   factory line is L1 = 53.75 nits in the dark and L2 = 170.89 nits at
 *   E2 = 262.5 lux.
 * - A new target within 10% of what is shown is ignored, so small changes in
 *   the light don't move the screen.
 * - Changes run as ramps in perceptual brightness, on a log scale between the
 *   panel's darkest and brightest levels. Dimming takes 5 s. Brightening takes
 *   1.5 s once the step is a tenth of the perceptual range or more, and 5 s
 *   for steps of up to 0.04, with a linear blend between the two.
 * - A brightness the user picks (the desktop's slider or the keys) is learned:
 *   the line is moved to pass through it. Below 78.5 lux the dark end moves;
 *   above it, the bright end does. The line is saved, so it survives
 *   reboots.
 *
 * Other changes, such as the desktop's idle dimming or the brightness it
 * restores when a session starts, are respected without being learned: the
 * level holds until it returns to the automatic one or the room's light
 * changes about fourfold. A change counts as the user's when a key, the
 * touchpad or a mouse was used just before it.
 *
 * The backlight's brightness is taken to be in nits, as the Apple DCP driver
 * reports it for the built-in panel. Only sysfs and evdev are used, so this needs nothing beyond libc.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_MS 200		/* sensor read: the AOP reports at 5 Hz */
#define RAMP_TICK_MS 33		/* backlight steps while ramping */
#define MAX_LUX 5000.0		/* the curve's lux cap */
#define HOLD_RATIO 4.0		/* light change that ends a hold */
#define USER_INPUT_S 2.0	/* input this recent: a change is the user's */
#define START_GRACE_S 30.0	/* after start or wake, changes are never learned */
#define SETTLE_S 1.0		/* someone else's change: wait until the level stays this long */
#define DISPLAYS_OFF_S 1.0	/* displays dark this long: leave the panel alone */
#define RESCAN_S 30		/* look for new input devices this often */
#define MAX_INPUTS 16
#define LONG_BITS (8 * sizeof(long))

struct config {
	double dark_nits;	/* L1: the line at 0 lux */
	double ref_lux;		/* E2 */
	double ref_nits;	/* L2: the line at E2 */
	double pivot_lux;	/* learning below this moves L1, above it L2 */
	double min_nits;	/* the panel's darkest level: the perceptual scale's floor */
	double deadband;	/* relative change below which a target is ignored */
	double dim_s;		/* ramp length when dimming */
	double brighten_s;	/* ramp length for a large brightening step */
	bool learn;
	bool verbose;
};

struct curve {
	double l1, l2;		/* nits at 0 lux and at ref_lux */
};

struct ramp {
	bool active;
	double from, to;	/* perceptual */
	double start, length;	/* s */
};

static volatile sig_atomic_t stop;
static int input_fd[MAX_INPUTS];
static int input_count;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static double env_double(const char *name, double def)
{
	const char *v = getenv(name);
	char *end;
	double d;

	if (!v || !*v)
		return def;
	d = strtod(v, &end);
	return (*end || d < 0) ? def : d;
}

static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -errno;
	buf[n] = '\0';
	return 0;
}

static int read_int(const char *path, long *out)
{
	char buf[64], *end;
	int ret = read_file(path, buf, sizeof(buf));

	if (ret)
		return ret;
	*out = strtol(buf, &end, 10);
	return end == buf ? -EINVAL : 0;
}

static int write_int(const char *path, long value)
{
	char buf[32];
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	int len, ret = 0;

	if (fd < 0)
		return -errno;
	len = snprintf(buf, sizeof(buf), "%ld\n", value);
	if (write(fd, buf, len) != len)
		ret = -errno;
	close(fd);
	return ret;
}

/*
 * The built-in panel's backlight: AURORA_DISPLAY_LIGHT_BACKLIGHT if set, else
 * the first platform or firmware one. The Apple DCP driver registers the
 * built-in panel as a platform backlight, in nits, and external displays as raw
 * ones.
 */
static char *find_backlight(void)
{
	static const char *const types[] = { "platform", "firmware" };
	const char *name = getenv("AURORA_DISPLAY_LIGHT_BACKLIGHT");
	char *path = NULL;
	glob_t g;

	if (name && *name) {
		char p[512];

		snprintf(p, sizeof(p), "/sys/class/backlight/%s", name);
		return access(p, F_OK) ? NULL : strdup(p);
	}
	if (glob("/sys/class/backlight/*", 0, NULL, &g) != 0)
		return NULL;
	for (size_t t = 0; t < sizeof(types) / sizeof(types[0]) && !path; t++)
		for (size_t i = 0; i < g.gl_pathc && !path; i++) {
			char p[512], buf[32];

			snprintf(p, sizeof(p), "%s/type", g.gl_pathv[i]);
			if (!read_file(p, buf, sizeof(buf)) && !strncmp(buf, types[t], strlen(types[t])))
				path = strdup(g.gl_pathv[i]);
		}
	globfree(&g);
	return path;
}

/* The first IIO device with a processed illuminance channel. */
static char *find_als(void)
{
	glob_t g;
	char *path = NULL;

	if (glob("/sys/bus/iio/devices/iio:device*/in_illuminance_input", 0, NULL, &g) == 0 &&
	    g.gl_pathc)
		path = strdup(g.gl_pathv[0]);
	globfree(&g);
	return path;
}

static int read_lux(const char *path, double *lux)
{
	char buf[64], *end;
	int ret = read_file(path, buf, sizeof(buf));

	if (ret)
		return ret;
	*lux = strtod(buf, &end);
	return (end == buf || *lux < 0) ? -EINVAL : 0;
}

static bool test_bit(const unsigned long *bits, int bit)
{
	return (bits[bit / LONG_BITS] >> (bit % LONG_BITS)) & 1;
}

/* An event device reporting the lid switch, or -1. */
static int open_lid(void)
{
	unsigned long bits[(SW_MAX + LONG_BITS) / LONG_BITS];
	struct dirent *de;
	DIR *dir = opendir("/dev/input");
	int fd = -1;

	if (!dir)
		return -1;
	while ((de = readdir(dir)) && fd < 0) {
		char path[300];
		int cand;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		cand = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (cand < 0)
			continue;
		memset(bits, 0, sizeof(bits));
		if (ioctl(cand, EVIOCGBIT(EV_SW, sizeof(bits)), bits) >= 0 && test_bit(bits, SW_LID))
			fd = cand;
		else
			close(cand);
	}
	closedir(dir);
	return fd;
}

static bool lid_closed(int fd)
{
	unsigned long bits[(SW_MAX + LONG_BITS) / LONG_BITS] = { 0 };

	if (fd < 0 || ioctl(fd, EVIOCGSW(sizeof(bits)), bits) < 0)
		return false;
	return test_bit(bits, SW_LID);
}

static void close_inputs(void)
{
	for (int i = 0; i < input_count; i++)
		if (input_fd[i] >= 0)
			close(input_fd[i]);
	input_count = 0;
}

/* Open every keyboard (the A key or brightness keys) and pointer (a touch or a left button). */
static void open_inputs(void)
{
	unsigned long bits[(KEY_MAX + LONG_BITS) / LONG_BITS];
	struct dirent *de;
	DIR *dir;

	close_inputs();
	dir = opendir("/dev/input");
	if (!dir)
		return;
	while ((de = readdir(dir)) && input_count < MAX_INPUTS) {
		char path[300];
		int fd;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
			continue;
		memset(bits, 0, sizeof(bits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
		    (test_bit(bits, KEY_A) || test_bit(bits, KEY_BRIGHTNESSDOWN) ||
		     test_bit(bits, BTN_TOUCH) || test_bit(bits, BTN_LEFT)))
			input_fd[input_count++] = fd;
		else
			close(fd);
	}
	closedir(dir);
}

/* True when every connected display is powered off, as when the desktop blanks the screens. */
static bool displays_off(void)
{
	int connected = 0, on = 0;
	glob_t g;

	if (glob("/sys/class/drm/card*-*/status", 0, NULL, &g) != 0)
		return false;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		char buf[32], dpms[512];
		size_t len = strlen(g.gl_pathv[i]) - strlen("status");

		if (read_file(g.gl_pathv[i], buf, sizeof(buf)) || strncmp(buf, "connected", 9))
			continue;
		connected++;
		snprintf(dpms, sizeof(dpms), "%.*sdpms", (int)len, g.gl_pathv[i]);
		if (read_file(dpms, buf, sizeof(buf)) || !strncmp(buf, "On", 2))
			on++;
	}
	globfree(&g);
	return connected && !on;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* How far CLOCK_BOOTTIME is ahead of CLOCK_MONOTONIC: grows across a suspend. */
static double suspended_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9 - now_s();
}

/* Wait ms, noting the time of the last key, touch or button event in *last_input. */
static void wait_ms(int ms, double *last_input)
{
	double end = now_s() + ms / 1000.0;
	struct pollfd p[MAX_INPUTS];

	while (!stop) {
		int left = (int)ceil((end - now_s()) * 1000);

		if (left <= 0)
			return;
		for (int i = 0; i < input_count; i++)
			p[i] = (struct pollfd){ .fd = input_fd[i], .events = POLLIN };
		if (poll(p, input_count, left) <= 0)
			return;
		for (int i = 0; i < input_count; i++) {
			struct input_event ev[64];
			ssize_t len;

			if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				close(input_fd[i]);	/* unplugged */
				input_fd[i] = -1;
				continue;
			}
			if (!(p[i].revents & POLLIN))
				continue;
			while ((len = read(input_fd[i], ev, sizeof(ev))) > 0)
				for (size_t j = 0; j < (size_t)len / sizeof(ev[0]); j++)
					if (ev[j].type == EV_KEY || ev[j].type == EV_REL || ev[j].type == EV_ABS)
						*last_input = now_s();
		}
	}
}

/* macOS's LuminanceToPerceptual: 0 at the darkest level, 1 at the brightest, log in between. */
static double to_perceptual(double nits, double lo, double hi)
{
	if (nits > hi)
		nits = hi;
	if (nits < lo)
		nits = lo;
	return 1.0 + log(nits / hi) / log(hi / lo);
}

static double from_perceptual(double p, double lo, double hi)
{
	if (p > 1)
		p = 1;
	if (p < 0)
		p = 0;
	return hi * pow(hi / lo, p - 1.0);
}

static double curve_nits(const struct config *c, const struct curve *k, double lux, double max)
{
	double n;

	if (lux > MAX_LUX)
		lux = MAX_LUX;
	n = k->l1 + (k->l2 - k->l1) * lux / c->ref_lux;
	if (n > max)
		n = max;
	if (n < c->min_nits)
		n = c->min_nits;
	return n;
}

/*
 * Ramp length for a step in perceptual brightness: macOS's AnimationRateTable,
 * 5 s for any dimming and for brightening up to 0.04, 1.5 s from 0.1 up.
 */
static double ramp_length(const struct config *c, double dp)
{
	if (dp <= 0.04)
		return c->dim_s;
	if (dp >= 0.10)
		return c->brighten_s;
	return c->dim_s + (c->brighten_s - c->dim_s) * (dp - 0.04) / 0.06;
}

/*
 * Move the line through (lux, nits), so the level the user chose is the one
 * the line gives there. In a dim room the dark end moves and the bright end
 * stays; in a bright room the other way round. The line never falls as the
 * light rises, and its dark end never goes below the panel's darkest level:
 * where the dark end would, the bright end moves instead.
 */
static void learn(const struct config *c, struct curve *k, double lux, double nits)
{
	if (lux > MAX_LUX)
		lux = MAX_LUX;
	if (nits < c->min_nits)
		nits = c->min_nits;
	if (lux <= c->pivot_lux) {
		double slope = (k->l2 - nits) / (c->ref_lux - lux);

		if (slope < 0) {
			k->l1 = k->l2 = nits;
			return;
		}
		k->l1 = nits - slope * lux;
	} else {
		double slope = (nits - k->l1) / lux;

		if (slope < 0) {
			k->l1 = k->l2 = nits;
			return;
		}
		k->l2 = k->l1 + slope * c->ref_lux;
	}
	if (k->l1 < c->min_nits) {
		/* pivot on the darkest level instead (lux > 0 here: at 0 lx l1 is the point) */
		k->l1 = c->min_nits;
		k->l2 = c->min_nits + (nits - c->min_nits) * c->ref_lux / lux;
	}
}

static void state_path(char *buf, size_t len)
{
	const char *dir = getenv("STATE_DIRECTORY");	/* systemd's StateDirectory= */

	snprintf(buf, len, "%s/curve", dir && *dir ? dir : "/var/lib/aurora-display-light");
}

static void load_curve(struct curve *k)
{
	char path[512], buf[128];
	double l1, l2;

	state_path(path, sizeof(path));
	if (!read_file(path, buf, sizeof(buf)) && sscanf(buf, "%lf %lf", &l1, &l2) == 2 &&
	    l1 > 0 && l2 > 0) {
		k->l1 = l1;
		k->l2 = l2;
	}
}

static void save_curve(const struct curve *k)
{
	char path[512], tmp[520], buf[128];
	int fd, len;

	state_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	len = snprintf(buf, sizeof(buf), "%.3f %.3f\n", k->l1, k->l2);
	if (write(fd, buf, len) == len && fsync(fd) == 0)
		rename(tmp, path);
	close(fd);
}

int main(int argc, char **argv)
{
	struct config c = {
		.dark_nits = env_double("AURORA_DISPLAY_LIGHT_DARK_NITS", 53.75),
		.ref_lux = env_double("AURORA_DISPLAY_LIGHT_REF_LUX", 262.5),
		.ref_nits = env_double("AURORA_DISPLAY_LIGHT_REF_NITS", 170.89),
		.pivot_lux = env_double("AURORA_DISPLAY_LIGHT_PIVOT_LUX", 78.5),
		.min_nits = env_double("AURORA_DISPLAY_LIGHT_MIN_NITS", 2),
		.deadband = env_double("AURORA_DISPLAY_LIGHT_DEADBAND", 0.10),
		.dim_s = env_double("AURORA_DISPLAY_LIGHT_DIM_SECONDS", 5),
		.brighten_s = env_double("AURORA_DISPLAY_LIGHT_BRIGHTEN_SECONDS", 1.5),
		.learn = env_double("AURORA_DISPLAY_LIGHT_LEARN", 1) != 0,
	};
	char *bl_dir, *als_path, bright_path[512], max_path[512];
	struct curve k;
	struct ramp r = { 0 };
	double lux = NAN, shown = -1, hold_lux = 0, last_input = -1e9, grace_until;
	double lid_retry_t, scan_t, sample_t = 0, dark_since = -1, suspended;
	long max, cur, written = -1, auto_last = -1;
	double ext_t = -1;	/* someone else's change in progress: when the level last moved */
	bool hold = false, ext_user = false;
	int lid_fd;

	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			c.verbose = true;
	if (!(c.ref_lux > 0 && c.min_nits > 0 && c.dim_s > 0 && c.brighten_s > 0)) {
		fprintf(stderr, "invalid settings\n");
		return 2;
	}
	k.l1 = c.dark_nits;
	k.l2 = c.ref_nits;
	if (c.learn)
		load_curve(&k);

	bl_dir = find_backlight();
	als_path = find_als();
	if (!bl_dir || !als_path) {
		fprintf(stderr, "no %s; nothing to do\n", bl_dir ? "ambient light sensor" : "backlight");
		return 0;
	}
	snprintf(bright_path, sizeof(bright_path), "%s/brightness", bl_dir);
	snprintf(max_path, sizeof(max_path), "%s/max_brightness", bl_dir);
	if (read_int(max_path, &max) || max <= 0) {
		fprintf(stderr, "cannot read %s\n", max_path);
		return 1;
	}
	if (c.min_nits >= max)
		c.min_nits = 1;
	lid_fd = open_lid();
	open_inputs();
	fprintf(stderr,
		"backlight %s (max %ld nits), light sensor %s, lid switch %s, %d input devices; "
		"line %.1f nits at 0 lx to %.1f nits at %.1f lx%s\n",
		bl_dir, max, als_path, lid_fd >= 0 ? "found" : "not found", input_count,
		k.l1, k.l2, c.ref_lux, c.learn ? ", learning" : "");

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	lid_retry_t = scan_t = now_s();
	grace_until = lid_retry_t + START_GRACE_S;
	suspended = suspended_s();

	while (!stop) {
		double t = now_s(), s = suspended_s(), target, base;
		bool dark;

		if (lid_fd < 0 && t - lid_retry_t > 30) {
			lid_retry_t = t;
			lid_fd = open_lid();
		}
		if (t - scan_t > RESCAN_S) {
			scan_t = t;
			open_inputs();
		}
		/* After a wake the desktop restores its own level: don't learn it, start over. */
		if (s - suspended > 2) {
			grace_until = t + START_GRACE_S;
			written = -1;
			hold = false;
			ext_t = -1;
			r.active = false;
		}
		suspended = s;

		if (!displays_off())
			dark_since = -1;
		else if (dark_since < 0)
			dark_since = t;
		dark = lid_closed(lid_fd) || (dark_since >= 0 && t - dark_since >= DISPLAYS_OFF_S);
		if (dark || read_int(bright_path, &cur) || cur <= 0) {
			/* Off, or the panel is dark: leave it, and take the level as found after. */
			written = -1;
			ext_t = -1;
			r.active = false;
			wait_ms(SAMPLE_MS, &last_input);
			continue;
		}

		if (t - sample_t >= SAMPLE_MS / 1000.0 - 0.005) {
			double l;

			sample_t = t;
			if (read_lux(als_path, &l) == 0)
				lux = l;
		}
		if (isnan(lux)) {
			wait_ms(SAMPLE_MS, &last_input);
			continue;
		}

		/*
		 * Someone else set the level. A desktop animates its own changes (KDE
		 * steps every ~40 ms for a second or so): follow the level until it
		 * stays put, then take the level it ended at, once. It is the user's
		 * if there was keyboard or pointer input while it moved.
		 */
		if (written >= 0 && cur != written) {
			if (ext_t < 0)
				ext_user = false;
			ext_t = t;
			ext_user |= t - last_input <= USER_INPUT_S && t >= grace_until;
			r.active = false;
			shown = cur;
			written = cur;
		}
		if (ext_t >= 0) {
			if (t - ext_t < SETTLE_S) {
				wait_ms(SAMPLE_MS, &last_input);
				continue;
			}
			ext_t = -1;
			if (cur == auto_last) {
				hold = false;	/* restored after idle dimming */
				if (c.verbose)
					fprintf(stderr, "restored to %ld: automatic\n", cur);
			} else if (ext_user && c.learn) {
				learn(&c, &k, lux, cur);
				save_curve(&k);
				hold = false;
				if (c.verbose)
					fprintf(stderr, "user set %ld nits at %.1f lx: line now %.1f to %.1f nits\n",
						cur, lux, k.l1, k.l2);
			} else {
				hold = true;
				hold_lux = lux;
				if (c.verbose)
					fprintf(stderr, "set to %ld by someone else at %.1f lx: holding\n", cur, lux);
			}
		}
		if (written < 0) {
			written = cur;	/* start from what is set */
			shown = cur;
		}
		if (hold) {
			if (fabs(log10((lux + 1) / (hold_lux + 1))) <= log10(HOLD_RATIO)) {
				wait_ms(SAMPLE_MS, &last_input);
				continue;
			}
			hold = false;
			if (c.verbose)
				fprintf(stderr, "light changed to %.1f lx: automatic\n", lux);
		}

		/* A new target, against what is shown now (mid-ramp included). */
		target = curve_nits(&c, &k, lux, max);
		base = r.active ? from_perceptual(r.from + (r.to - r.from) *
						  fmin(1, (t - r.start) / r.length), c.min_nits, max)
				: shown;
		if (base > 0 && fabs(target - base) / base >= c.deadband &&
		    (!r.active || fabs(target - from_perceptual(r.to, c.min_nits, max)) / base >= c.deadband)) {
			r.from = to_perceptual(base, c.min_nits, max);
			r.to = to_perceptual(target, c.min_nits, max);
			r.start = t;
			r.length = ramp_length(&c, r.to - r.from);
			r.active = true;
			if (c.verbose)
				fprintf(stderr, "%.1f lx: %.0f -> %.0f nits over %.1f s\n", lux, base, target,
					r.length);
		}

		if (r.active) {
			double f = fmin(1, (t - r.start) / r.length);
			long next;

			shown = from_perceptual(r.from + (r.to - r.from) * f, c.min_nits, max);
			next = lround(shown);
			if (next < 1)
				next = 1;
			if (next != written && write_int(bright_path, next) == 0) {
				written = next;
				auto_last = next;
			}
			if (f >= 1)
				r.active = false;
			wait_ms(RAMP_TICK_MS, &last_input);
		} else {
			wait_ms(SAMPLE_MS, &last_input);
		}
	}
	close_inputs();
	if (lid_fd >= 0)
		close(lid_fd);
	return 0;
}
