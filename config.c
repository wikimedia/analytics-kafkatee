#define _ISOC99_SOURCE  /* for strtoull() */
#define _GNU_SOURCE     /* for strdupa() */

#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>

#include <librdkafka/rdkafka.h>

#include "kafkatee.h"
#include "input.h"
#include "output.h"
#include "ezd.h"

/**
 * kafkatee global configuration
 */
struct conf conf;




/* Left trim string '*s' of white spaces and return the new position.
 * Does not modify the string. */
static char *ltrim (const char *s) {
	while (isspace(*s))
		s++;

	return (char *)s;
}



/**
 * Parses the value as true or false.
 */
static int conf_tof (const char *val) {
	char *end;
	int i;
	
	i = strtoul(val, &end, 0);
	if (end > val) /* treat as integer value */
		return !!i;

	if (!strcasecmp(val, "yes") ||
	    !strcasecmp(val, "true") ||
	    !strcasecmp(val, "on") ||
	    !*val /* empty value is true */)
		return 1;
	else
		return 0;
}

static encoding_t encoding_parse (const char *val) {
	if (!strcasecmp(val, "string"))
		return ENC_STRING;
	else if (!strcasecmp(val, "json"))
		return ENC_JSON;
	else
		return ENC_ERROR;
}

static char *str_unescape (char *str) {
	char *s = str;

	while (*s) {
		int esc = -1;
		if (!strncmp(s, "\\n", 2))
			esc = '\n';
		else if (!strncmp(s, "\\r", 2))
			esc = '\r';
		else if (!strncmp(s, "\\t", 2))
			esc = '\t';
		else if (!strncmp(s, "\\0", 2))
			esc = '\0';

		if (esc != -1) {
			*s = esc;
			memmove(s+1, s+2, strlen(s+2)+1);
		}

		s++;
	}

	return str;
}

/**
 * Parses a "k=v,k2=v2,.." string in 'str' (which is modified)
 * and returns 1 on match, else 0.
 * Key name and value are returned in 'namep' and 'valp'.
 * If no value if specified (and no '='), then value is set to "".
 * '*strp' is forwarded to the next token on return for a sub-sequent
 * call to kv_parse() (if return is 1).
 */
static int kv_parse (char **strp, const char **namep, const char **valp) {
	char *s;
	char *str = *strp;

	if (!str)
		return 0;

	while (isspace((int)*str))
		str++;

	if (!*str)
		return 0;

	if (!(s = strchr(str, '='))) {
		*namep = str;
		*valp = " ";
		*strp = NULL; /* EOF */
		return 1;
	}

	*s = '\0';
	*namep = str;
	*valp = s+1;

	if ((s = strchr(*valp, ','))) {
		*s = '\0';
		*strp = s+1;
	} else
		*strp = NULL;

	return 1;
}


/**
 * Parse input key-values.
 */
static int input_kvs_parse (char *str, encoding_t *encp, int *flagsp,
			    char *errstr, size_t errstr_size) {

	const char *kv_n, *kv_v;

	while (kv_parse(&str, &kv_n, &kv_v)) {
		if (!strcmp(kv_n, "encoding")) {
			if ((*encp = encoding_parse(kv_v)) == ENC_ERROR) {
				snprintf(errstr, errstr_size,
					 "Invalid encoding %s", kv_v);
				return -1;
			}
		} else if (!strcmp(kv_n, "stop.eof")) {
			if (conf_tof(kv_v))
				*flagsp |= INPUT_F_STOP_EOF;
			else
				*flagsp &= ~INPUT_F_STOP_EOF;
		} else if (!strcmp(kv_n, "stop.error")) {
			if (conf_tof(kv_v))
				*flagsp |= INPUT_F_STOP_ERROR;
			else
				*flagsp &= ~INPUT_F_STOP_ERROR;
		} else if (!strcmp(kv_n, "exit.on.exit")) {
			if (conf_tof(kv_v))
				*flagsp |= INPUT_F_EXIT_ON_EXIT;
			else
				*flagsp &= ~INPUT_F_EXIT_ON_EXIT;
		} else {
			snprintf(errstr, errstr_size,
				 "Unknown option: %s", kv_n);
			return -1;
		}
	}

	return 0;
}


/**
 * Set a single configuration property 'name' using value 'val'.
 * Returns 0 on success, and -1 on error in which case 'errstr' will
 * contain an error string.
 */
int conf_set (const char *name, const char *val,
	      char *errstr, size_t errstr_size,
	      void *opaque) {
	rd_kafka_conf_res_t res;

	/* Kafka configuration */
	if (!strncmp(name, "kafka.", strlen("kafka."))) {
		name += strlen("kafka.");

		/* Kafka topic configuration. */
		if (!strncmp(name, "topic.", strlen("topic.")))
			res = rd_kafka_topic_conf_set(conf.rkt_conf,
						      name+strlen("topic."),
						      val,
						      errstr, errstr_size);
		else /* Kafka global configuration */
			res = rd_kafka_conf_set(conf.rk_conf, name,
						val, errstr, errstr_size);

		if (res == RD_KAFKA_CONF_OK)
			return 0;
		else if (res != RD_KAFKA_CONF_UNKNOWN)
			return -1;
		
		/* Unknown configs: fall thru */
		name -= strlen("kafka.");
	}

	/* Non "key=value" config properties */
	if (!val) {
		struct iovec arg[8+1];

		if (ezd_regmatch("^input +(\\[([^\\]+)\\] +)?pipe +(.+)$",
				 name, arg, 3) == 3) {
			encoding_t enc = ENC_STRING;
			int flags = INPUT_F_DEFAULTS;

			/* Optional: [k=v,k2=v2,..] key-value pairs */
			if (arg[1].iov_base) {
				if (input_kvs_parse(ezd_strndupa_iov(&arg[1]),
						    &enc, &flags,
						    errstr, errstr_size) == -1)
					return -1;
			}

			if (!input_add(INPUT_PIPE, enc, flags,
				       ltrim(ezd_strndupa_iov(&arg[2])), 0, 0,
				       errstr, errstr_size))
				return -1;

		} else if (ezd_regmatch("^input +(\\[([^\\]+)\\] +)?kafka +"
					"topic +([^ ]+) +"
					"partition +([0-9]+)(-([0-9]+))?"
					"( +from "
					"+(beginning|end|stored|[0-9]+))"
					"$",
					name, arg, 8) >= 4) {
			int part_lo, part_hi;
			char *topic;
			int64_t offset = RD_KAFKA_OFFSET_STORED;
			encoding_t enc = ENC_STRING;
			int flags = INPUT_F_DEFAULTS;

			if (arg[1].iov_base) {
				if (input_kvs_parse(ezd_strndupa_iov(&arg[1]),
						    &enc, &flags,
						    errstr, errstr_size) == -1)
					return -1;
			}

			if (arg[7].iov_base) {
				if (!strncmp(arg[7].iov_base, "beginning", 9))
					offset = RD_KAFKA_OFFSET_BEGINNING;
				else if (!strncmp(arg[7].iov_base, "end", 3))
					offset = RD_KAFKA_OFFSET_END;
				else if (!strncmp(arg[7].iov_base, "stored", 6))
					offset = RD_KAFKA_OFFSET_STORED;
				else
					offset = strtoull(ezd_strndupa_iov(
								  &arg[7]),
							  NULL, 10);
			}

			topic = ezd_strndupa_iov(&arg[2]);

			part_lo = atoi(ezd_strndupa_iov(&arg[3]));
			if (arg[5].iov_base)
				part_hi = atoi(ezd_strndupa_iov(&arg[5]));
			else
				part_hi = part_lo;

			for ( ; part_lo <= part_hi ; part_lo++)
				if (!input_add(INPUT_KAFKA, enc, flags,
					       topic, part_lo,
					       offset, errstr, errstr_size))
					return -1;


		} else if (ezd_regmatch("^output +pipe +([0-9]+) +(.+)$",
					name, arg, 2) == 2) {
			output_add(OUTPUT_PIPE, atoi(ezd_strndupa_iov(&arg[0])),
				   ezd_strndupa_iov(&arg[1]));

		} else if (ezd_regmatch("^output +file +([0-9]+) +(.+)$",
					name, arg, 2) == 2) {
			output_add(OUTPUT_FILE, atoi(ezd_strndupa_iov(&arg[0])),
				   ezd_strndupa_iov(&arg[1]));

		} else {
			snprintf(errstr, errstr_size,
				 "Unknown configuration directive \"%s\"",
				 name);
			return -1;
		}

		return 0;
	}

	/* kafkatee configuration options */
	if (!strcmp(name, "output.format")) {
		if (conf.fconf.format)
			free(conf.fconf.format);
		conf.fconf.format = strdup(val);
	} else if (!strcmp(name, "output.encoding")) {
		if ((conf.fconf.encoding =
		     encoding_parse(val)) == ENC_ERROR) {
			snprintf(errstr, errstr_size,
				 "Unknown %s value \"%s\"", name, val);
			return -1;
		}
	} else if (!strcmp(name, "output.delimiter")) {
		if (conf.output_delimiter)
			free(conf.output_delimiter);
		conf.output_delimiter = str_unescape(strdup(val));
		conf.output_delimiter_len = strlen(conf.output_delimiter);
	} else if (!strcmp(name, "output.queue.size")) {
		conf.output_queue_size = atoi(val);
	} else if (!strcmp(name, "log.level"))
		conf.log_level = atoi(val);
	else if (!strcmp(name, "log.statistics.file")) {
		free(conf.stats_file);
		conf.stats_file = strdup(val);
	} else if (!strcmp(name, "log.statistics.interval"))
		conf.stats_interval = atoi(val);
	else if (!strcmp(name, "log.rate.max"))
		conf.log_rate = atoi(val);
	else if (!strcmp(name, "log.rate.period"))
		conf.log_rate_period = atoi(val);
	else if (!strcmp(name, "daemonize"))
		conf.daemonize = conf_tof(val);
	else if (!strcmp(name, "pid.file.path")) {
		free(conf.pid_file_path);
		conf.pid_file_path = strdup(val);
	} else if (!strcmp(name, "command.init")) {
		if (conf.cmd_init)
			free(conf.cmd_init);
		conf.cmd_init = strdup(val);
	} else if (!strcmp(name, "command.term")) {
		if (conf.cmd_term)
			free(conf.cmd_term);
		conf.cmd_term = strdup(val);
	} else if (!strncmp(name, "env.", strlen("env."))) {
		if (*val)
			setenv(name+strlen("env."), val, 1);
		else
			unsetenv(name+strlen("env."));
	} else {
		snprintf(errstr, errstr_size,
			 "Unknown configuration property \"%s\"\n", name);
		return -1;
	}


	return 0;
}

