/*
  Copyright (c) 2016, Matthias Schiffer <mschiffer@universe-factory.net>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <libplatforminfo.h>
#include <uci.h>

#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>


const char *const download_d_dir = "/usr/lib/autoupdater/download.d";
const char *const abort_d_dir = "/usr/lib/autoupdater/abort.d";
const char *const upgrade_d_dir = "/usr/lib/autoupdater/upgrade.d";


static struct globals {
	/* Settings (UCI config and command line) */
	bool force;
	bool fallback;

	const char *branch;

	/* Runtime values */
	struct uci_context *ctx;

	char *old_version;

	size_t n_mirrors;
	const char **mirrors;

	size_t n_pubkeys;
	const char **pubkeys;

	unsigned long good_signatures;
	const char *branch_name;
} G = {};


static char * read_one_line(const char *filename) {
        FILE *f = fopen(filename, "r");
        if (!f)
                return NULL;

        char *line = NULL;
        size_t len = 0;

        ssize_t r = getline(&line, &len, f);

        fclose(f);

        if (r >= 0) {
                len = strlen(line);

                if (len && line[len-1] == '\n')
                        line[len-1] = 0;
        }
        else {
                free(line);
                line = NULL;
        }

        return line;
}


static void usage(void) {
        /* TODO */
	fputs("Usage: autoupdater <foo>\n", stderr);
}


static void parse_args(int argc, char *argv[]) {
	enum option_values {
		OPTION_BRANCH = 'b',
		OPTION_FORCE = 'f',
		OPTION_HELP = 'h',
		OPTION_FALLBACK = 256,
	};

	const struct option options[] = {
		{"branch",   required_argument, NULL, OPTION_BRANCH},
		{"force",    no_argument,       NULL, OPTION_FORCE},
		{"fallback", no_argument,       NULL, OPTION_FALLBACK},
		{"help",     no_argument,       NULL, OPTION_HELP},
	};

	while (true) {
		int c = getopt_long(argc, argv, "b:fh", options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case OPTION_BRANCH:
			G.branch = optarg;

		case OPTION_FORCE:
			G.force = true;

		case OPTION_FALLBACK:
			G.fallback = true;

		case OPTION_HELP:
			usage();
			exit(0);

		default:
			usage();
			exit(1);
		}
	}
}


static unsigned long load_positive_number(struct uci_section *s, const char *option) {
	const char *str = uci_lookup_option_string(G.ctx, s, option);
	if (!str) {
		fprintf(stderr, "autoupdater: error: unable to load option '%s'\n", option);
		exit(1);
	}

	char *end;
	unsigned long ret = strtoul(str, &end, 0);
	if (*end || !ret) {
		fprintf(stderr, "autoupdater: error: invalid value for option '%s'\n", option);
		exit(1);
	}

	return ret;
}


static const char ** load_string_list(struct uci_section *s, const char *option, size_t *len) {
	struct uci_option *o = uci_lookup_option(G.ctx, s, option);
	if (!o) {
		fprintf(stderr, "autoupdater: error: unable to load option '%s'\n", option);
		exit(1);
	}

	if (o->type != UCI_TYPE_LIST) {
		fprintf(stderr, "autoupdater: error: invalid value for option '%s'\n", option);
		exit(1);
	}

	size_t i = 0;
	struct uci_element *e;
	uci_foreach_element(&o->v.list, e)
		i++;

	*len = i;
	const char **ret = malloc(i * sizeof(char *));

	i = 0;
	uci_foreach_element(&o->v.list, e)
		ret[i++] = uci_to_option(e)->v.string;

	return ret;
}


static void load_settings(void) {
	G.ctx = uci_alloc_context();
	G.ctx->flags &= ~UCI_FLAG_STRICT;

	struct uci_package *p;
	struct uci_element *e;
	struct uci_section *s;

	if (!uci_load(G.ctx, "autoupdater", &p)) {
		uci_foreach_element(&p->sections, e) {
	                s = uci_to_section(e);
	                if (strcmp(s->type, "settings") == 0)
				goto found;
	        }
	}

	fputs("autoupdater: error: unable to load UCI settings\n", stderr);
	exit(1);

 found:
	if (!G.branch)
		G.branch = uci_lookup_option_string(G.ctx, s, "branch");

	if (!G.branch) {
		fputs("autoupdater: error: no branch given in settings or command line\n", stderr);
		exit(1);
	}

	struct uci_section *branch = uci_lookup_section(G.ctx, p, G.branch);
	if (!branch || strcmp(branch->type, "branch")) {
		fputs("autoupdater: error: unable to load branch configuration\n", stderr);
		exit(1);
	}

	G.good_signatures = load_positive_number(branch, "good_signatures");
	G.mirrors = load_string_list(branch, "mirror", &G.n_mirrors);
	G.pubkeys = load_string_list(branch, "pubkey", &G.n_pubkeys);

	const char *version_file = uci_lookup_option_string(G.ctx, s, "version_file");
	if (version_file)
		G.old_version = read_one_line(version_file);

	const char *enabled = uci_lookup_option_string(G.ctx, s, "enabled");
	if ((!enabled || strcmp(enabled, "1")) && !G.force) {
		fputs("autoupdater is disabled\n", stderr);
		exit(0);
	}

	/* Don't free UCI context, we still reference values from it */
}


static void randomize(void) {
	struct timespec tv;
	if (clock_gettime(CLOCK_MONOTONIC, &tv)) {
		fprintf(stderr, "autoupdater: error: clock_gettime: %m\n");
		exit(1);
	}

	srandom(tv.tv_nsec);
}


static float get_uptime(void) {
	FILE *f = fopen("/proc/uptime", "r");
	if (f) {
		float uptime;
		int match = fscanf(f, "%f", &uptime);
		fclose(f);

		if (match == 1)
			return uptime;
	}

	fputs("autoupdater: error: unable to determine uptime\n", stderr);
	exit(1);
}


static float get_probability(time_t date, float priority) {
	float seconds = priority * 86400;
	time_t diff = time(NULL) - date;

	if (diff < 0) {
		/*
		 When the difference is negative, there are two possibilities: the
		 manifest contains an incorrect date, or our own clock is wrong. As there
		 isn't anything sensible to do for an incorrect manifest, we'll assume
		 the latter is the case and update anyways as we can't do anything better
		*/
		fputs("autoupdater: warning: clock seems to be incorrect.\n", stderr);

		if (get_uptime() < 600)
			/* If the uptime is very low, it's possible we just didn't get the time over NTP yet, so we'll just wait until the next time the updater runs */
			return 0;
		else
			/*
			 Will give 1 when priority == 0, and lower probabilities the higher
			 the priority value is (similar to the old static probability system)
			*/
			return powf(0.75f, priority);
	}
	else if (G.fallback) {
		if (diff >= seconds + 86400)
			return 1;
		else
			return 0;
	}
	else if (diff >= seconds) {
		return 1;
	}
	else {
		float x = diff/seconds;

		/*
		 This is the simplest polynomial with value 0 at 0, 1 at 1, and which has a
		 first derivative of 0 at both 0 and 1 (we all love continuously differentiable
		 functions, right?)
		*/
		return 3*x*x - 2*x*x*x;
	}
}


static void autoupdate(const char *mirror) {
}


static void lock_autoupdater(void) {
	const char *const lockfile = "/var/run/autoupdater.lock";

	int fd = open(lockfile, O_CREAT|O_RDONLY, 0666);
	if (fd < 0) {
		fprintf(stderr, "autoupdater: error: unable to open lock file: %m\n");
		exit(1);
	}

	if (flock(fd, LOCK_EX|LOCK_NB)) {
		fputs("autoupdater: error: another instance is currently running\n", stderr);
		exit(1);
	}
}


int main(int argc, char *argv[]) {
	parse_args(argc, argv);

	if (!platforminfo_get_image_name()) {
		fputs("autoupdater: error: unsupported hardware model\n", stderr);
		exit(1);
	}

	load_settings();
	randomize();

	lock_autoupdater();

	size_t mirrors_left = G.n_mirrors;
	while (mirrors_left) {
		const char **mirror = G.mirrors;
		size_t i = random() % mirrors_left;

		/* Move forward by i non-NULL entries */
		while (true) {
			while (!*mirror)
				mirror++;

			if (!i)
				break;

			mirror++;
			i--;
		}

		autoupdate(*mirror);

		/* When the update has failed, remove the mirror from the list */
		*mirror = NULL;
		mirrors_left--;
	}

	fputs("autoupdater: no usable mirror found\n", stderr);
	return 1;
}
