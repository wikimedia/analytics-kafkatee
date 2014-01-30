/*
 * Copyright (c) 2014 Wikimedia Foundation
 * Copyright (c) 2014 Magnus Edenhill <magnus@edenhill.se>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#define _GNU_SOURCE  /* for strndupa() */
#include <string.h>


#include <yajl/yajl_common.h>
#include <yajl/yajl_parse.h>
#include <yajl/yajl_version.h>

#if YAJL_MAJOR >= 2
#define YAJL_SIZE_TYPE size_t
#else
#define YAJL_SIZE_TYPE unsigned int
#endif


#include "kafkatee.h"



/**
 * Temporary scratch buffer
 */
struct tmpbuf {
	struct tmpbuf *next;
	size_t size;
	int    of;
	char   buf[0];  /* Must be at end of struct: allocated to '.size' */
};


/**
 * Resize the scratch buffer.
 * This is a destructive operation, the original data will be lost.
 */
static void render_scratch_resize (struct render *render, int size) {
	if (render->scratch_size < size) {
		if (render->scratch)
			free(render->scratch);
		
		render->scratch_size = size;
		render->scratch = malloc(size);
	}

	render->scratch_of = 0;
}


/**
 * Allocate space 'len' in scratch buffer.
 * The scratch buffer size is bound by its initial render_scratch_resize().
 */
static char *render_scratch_alloc (struct render *render, int len) {
	char *p;

	assert(render->scratch_of + len <= render->scratch_size);

	p = render->scratch + render->scratch_of;
	render->scratch_of  += len;

	return p;
}

/**
 * Find a %{field} formatter variable reference.
 */
static struct fmtvar *fmtvar_find (const struct fmt_conf *fconf,
				   const char *var, ssize_t varlen) {

	int i;

	for (i = 0 ; i < fconf->fmtvar_cnt ; i++) {
		struct fmtvar *fmtvar = &fconf->fmtvar[i];

		if (fmtvar->varlen == varlen &&
		    !strncmp(fmtvar->var, var, varlen))
			return fmtvar;
	}

	return NULL;
}

/**
 * Write value for %{field} formatter
 */
static void render_fmtval_write (struct render *render,
				 struct fmtval *fmtval,
				 const char *val, int vallen) {
	if (unlikely(fmtval->val != NULL))
		return;

	if (unlikely(!(fmtval->val = render_scratch_alloc(render, vallen))))
		return;

	memcpy(fmtval->val, val, vallen);
	fmtval->vallen = vallen;

}



/**
 * One renderer per input thread.
 */
static __thread struct render *thread_render = NULL;

/**
 * Destroys the thread-specific renderer.
 */
void render_destroy (void) {
	struct render *render = thread_render;

	if (!render)
		return;

	thread_render = NULL;

	if (render->scratch)
		free(render->scratch);
	free(render->fmtval);
	free(render);
}


/**
 * Create new thread-specific renderer.
 */
static struct render *render_new (const struct fmt_conf *fconf) {
	struct render *render;

	render = calloc(1, sizeof(*render));
	render->fmtval = calloc(sizeof(*render->fmtval), fconf->fmtvar_cnt);
	render->fconf  = fconf;

	assert(!thread_render);
	thread_render = render;

	return render;
}

static void render_reset (struct render *render) {
	memset(render->fmtval, 0,
	       sizeof(*render->fmtval) * render->fconf->fmtvar_cnt);
}

/**
 * All constant strings in the format are placed in 'const_string' which
 * hopefully will be small enough to fit a single cache line.
 */
#define CONST_STRING_SIZE 4096
static char         const_string[CONST_STRING_SIZE];
static size_t       const_string_len  = 0;

/**
 * Adds a constant string to the constant string area.
 * If the string is already found in the area, return it instead.
 */
static char *const_string_add (const char *in, int inlen) {
	char *ret;
	const char *instr = strndupa(in, inlen);
	
	if (!(ret = strstr(const_string, instr))) {
		assert(const_string_len + inlen < CONST_STRING_SIZE);

		/* Append new string */
		ret = const_string + const_string_len;
		memcpy(ret, in, inlen);
		ret[inlen] = '\0';
		const_string_len += inlen;
	}

	return ret;
}






/**
 * Render output to '*dstp' according to 'output.format' config.
 */
static int render_out (struct render *render, size_t maxlen,
		       void **dstp, size_t *dstlenp) {
	char *dst;
	int of = 0;
	int i;

	dst = malloc(maxlen);

	/* Traverse the formatter list */
	for (i = 0 ; i < render->fconf->fmt_cnt ; i++) {
		struct fmt *fmt = &render->fconf->fmt[i];
		struct fmtval *fmtval;
		const char *ptr;
		int len;

		if (fmt->varidx != -1 &&
		    (fmtval = &render->fmtval[fmt->varidx]) &&
		    fmtval->seen) {
			ptr = fmtval->val;
			len = fmtval->vallen;
		} else {
			ptr = fmt->def;
			len = fmt->deflen;
		}

		if (len == 0)
			continue;

		if (unlikely(of + len > maxlen)) {
			maxlen = (maxlen + 256) * 2;
			dst = realloc(dst, maxlen);
		}

		memcpy(dst+of, ptr, len);
		of += len;
	}

	*dstp = dst;
	*dstlenp = of;

	return 0;
}



/**
 * JSON parser callbacks (jp_..())
 */
struct json_parse_ctx {
	enum {
		JP_PARSE,  /* Parse next field */
		JP_SKIP,   /* Skip next value: key didnt match */
	} state;
	struct fmtval *fmtval; /* write RHS value to this val. */
	struct render *render; /* output rendering instance */
};


#define _JP_CHECK() do {			\
	if (jp->state == JP_SKIP) {		\
		jp->state = JP_PARSE;		\
		return 1;			\
	}					\
	} while (0)

static int jp_write (struct json_parse_ctx *jp, const char *val, size_t len) {
	if (unlikely(!jp->fmtval))
		return 1;

	render_fmtval_write(jp->render, jp->fmtval, val, len);
	jp->fmtval->seen++;
	jp->fmtval = NULL;

	return 1;
}

static int jp_bool (void *opaque, int val) {
	struct json_parse_ctx *jp = opaque;

	_JP_CHECK();

	if (val)
		return jp_write(jp, "true", 4);
	else
		return jp_write(jp, "false", 5);
}


static int jp_number (void *opaque, const char *numval,
                      YAJL_SIZE_TYPE numlen) {
	struct json_parse_ctx *jp = opaque;

	_JP_CHECK();

	return jp_write(jp, numval, numlen);
}


static int jp_string (void *opaque, const unsigned char *val,
                      YAJL_SIZE_TYPE len) {
	struct json_parse_ctx *jp = opaque;

	_JP_CHECK();

	return jp_write(jp, (const char *)val, len);
}


static int jp_start_map (void *opaque) {
	/* Maps are flattened for now */
	return 1;
}


static int jp_map_key (void *opaque, const unsigned char *key,
                       YAJL_SIZE_TYPE len) {
	struct json_parse_ctx *jp = opaque;
	const struct fmtvar *fmtvar;

	/* Left hand side: find matching formatter variable */
	if (!(fmtvar = fmtvar_find(jp->render->fconf,
				   (const char *)key, len))) {
		/* Formatter key not found: skip right hand side */
		jp->state = JP_SKIP;
		return 1;
	}

	jp->fmtval = &jp->render->fmtval[fmtvar->idx];
	jp->state = JP_PARSE;
	
	return 1;
}


static int jp_end_map (void *opaque) {
	return 1;
}


/**
 * Transform payload to JSON.
 */
static int payload_transform_from_json (void **dstp, size_t *dstlenp,
					const void *src, size_t srclen) {
	static const yajl_callbacks callbacks = {
		.yajl_boolean =   jp_bool,
		.yajl_number =    jp_number,
		.yajl_string =    jp_string,
		.yajl_start_map = jp_start_map,
		.yajl_map_key =   jp_map_key,
		.yajl_end_map =   jp_end_map,
	};
	yajl_handle yh;
	struct json_parse_ctx jp = {};
	yajl_status status;
	struct render *render;

	if (unlikely(!(render = thread_render)))
		render = render_new(&conf.fconf);
	else
		render_reset(render);

	/* JSON does not expand when transformed to string,
	 * so we can use the JSON size for the scratch buffer. */
	render_scratch_resize(render, srclen);

	jp.render = render;

	/* Set up JSON parser */
	yh = yajl_alloc(&callbacks, NULL,
#if YAJL_MAJOR < 2
                        NULL,
#endif
                        &jp);
#if YAJL_MAJOR >= 2
	yajl_config(yh, yajl_dont_validate_strings, 1);
	yajl_config(yh, yajl_allow_trailing_garbage, 1); /* e.g. newlines */
#endif

	/* Parse JSON */
	status = yajl_parse(yh, src, srclen);
	if (likely(status == yajl_status_ok)) {
#if YAJL_MAJOR >= 2
		status = yajl_complete_parse(yh);
#else
                status = yajl_parse_complete(yh);
#endif
        }

	/* Handle parsing errors, if any */
	if (unlikely(status == yajl_status_error)) {
		char *errstr = (char *)yajl_get_error(yh, 0, NULL, 0);
		if (errstr[strlen(errstr)-1] == '\n')
			errstr[strlen(errstr)-1] = '\0';
		kt_log(LOG_ERR, "JSON parse failed: %s: %.*s%s", errstr,
		       (int)(srclen > 200 ? 200 : srclen), (const char *)src,
		       srclen > 200 ? "..." : "");
		       

		yajl_free_error(yh, (unsigned char *)errstr);
		yajl_free(yh);
		return -1;
	}

	yajl_free(yh);

	
	return render_out(render, srclen, dstp, dstlenp);
}
	

/**
 * Transform payload from one encoding to another.
 * Returns -1 on error.
 */
int payload_transform (encoding_t in_enc,
		       void **dstp, size_t *dstlenp,
		       const void *src, size_t srclen) {

	assert(in_enc != conf.fconf.encoding);

	if (in_enc == ENC_JSON)
		return payload_transform_from_json(dstp, dstlenp, src, srclen);
	else
		return -1;
}




/**
 * Looks for any matching character from 'match' in 's' and returns
 * a pointer to the first match, or NULL if none of 'match' matched 's'.
 */
static char *strnchrs (const char *s, int len, const char *match) {
	const char *end = s + len;
	char map[256] = {};
	while (*match)
		map[(int)*(match++)] = 1;
	
	while (s < end) {
		if (map[(int)*s])
			return (char *)s;
		s++;
	}

	return NULL;
}
	



/**
 * Print parsed format string: formatters
 */
static __attribute__((unused)) void fmt_dump (const struct fmt_conf *fconf) {
	const struct fmtvar *fmtvar;
	int i;

	_DBG("%i/%i formats:",
	     fconf->fmt_cnt, fconf->fmt_size);
	for (i = 0 ; i < fconf->fmt_cnt ; i++) {
		if (fconf->fmt[i].varidx != -1)
			fmtvar = &fconf->fmtvar[fconf->fmt[i].varidx];
		else
			fmtvar = NULL;

		_DBG(" #%-3i \"%.*s\", varidx %i, def (%i)\"%.*s\"%s",
		     i,
		     fmtvar ? fmtvar->varlen : 0,
		     fmtvar ? fmtvar->var : NULL,
		     fconf->fmt[i].varidx,
		     fconf->fmt[i].deflen, fconf->fmt[i].deflen,
		     fconf->fmt[i].def,
		     fconf->fmt[i].flags & FMT_F_ESCAPE ? ", escape" : "");
	}

	_DBG("%i/%i fmtvars:", fconf->fmtvar_cnt, fconf->fmtvar_size);
	for (i = 0 ; i < fconf->fmtvar_cnt ; i++) {
		fmtvar = &fconf->fmtvar[i];
		_DBG(" #%i/%i: \"%.*s\"",
		     fmtvar->idx, i, fmtvar->varlen, fmtvar->var);
	}
}


/**
 * Adds a parsed formatter to the list of formatters
 */
static struct fmt *format_add (struct fmt_conf *fconf,
			       const char *var, ssize_t varlen,
			       const char *def, ssize_t deflen,
			       int flags,
			       char *errstr, size_t errstr_size) {
	struct fmt *fmt;

	if (fconf->fmt_cnt >= fconf->fmt_size) {
		fconf->fmt_size = (fconf->fmt_size ? : 32) * 2;
		fconf->fmt = realloc(fconf->fmt,
				     fconf->fmt_size * sizeof(*fconf->fmt));
	}

	fmt = &fconf->fmt[fconf->fmt_cnt];
	memset(fmt, 0, sizeof(*fmt));

	fmt->idx     = fconf->fmt_cnt;
	fmt->flags   = flags;
	fmt->varidx  = -1;

	if (!def)
		def = "-";

	if (deflen == -1)
		deflen = strlen(def);
	fmt->deflen = deflen;
	fmt->def = const_string_add(def, deflen);

	fconf->fmt_cnt++;

	return fmt;
}


/**
 * Adds a %{field} formatter reference.
 */
static struct fmtvar *fmtvar_add (struct fmt_conf *fconf,
				  const char *var, ssize_t varlen) {
	struct fmtvar *fmtvar;

	if (fconf->fmtvar_cnt >= fconf->fmtvar_size) {
		fconf->fmtvar_size = (fconf->fmtvar_size ? : 32) * 2;
		fconf->fmtvar = realloc(fconf->fmtvar,
					fconf->fmtvar_size *
					sizeof(*fconf->fmtvar));
	}

	fmtvar         = &fconf->fmtvar[fconf->fmtvar_cnt];
	fmtvar->var    = const_string_add(var, varlen);
	fmtvar->varlen = varlen;
	fmtvar->idx    = fconf->fmtvar_cnt++;


	return fmtvar;
}


/**
 * Parse the format string and build a parsing array.
 */
int format_parse (struct fmt_conf *fconf, const char *format_orig,
		  char *errstr, size_t errstr_size) {
	const char *s, *t;
	const char *format;
	int cnt = 0;

	/* Perform legacy replacements. */
	format = strdupa(format_orig);

	/* Parse the format string */
	s = t = format;
	while (*s) {
		const char *begin;
		const char *var = NULL;
		int varlen = 0;
		const char *def = "-";
		int deflen = 1;
		int flags = 0;
		const char *a;
		const char *b;
		const char *q;
		struct fmt *fmt;
		struct fmtvar *fmtvar;

		if (*s != '%') {
			s++;
			continue;
		}

		/* ".....%... "
		 *  ^---^  add this part as verbatim string */
		if (s > t)
			if (!format_add(fconf,
				       NULL, 0,
				       t, (int)(s - t),
				       0, errstr, errstr_size))
				return -1;

		begin = s;
		s++;

		if (*s != '{') {
			s++;
			continue;
		}


		/* Parse '{VAR}X': '*s' will be set to X, and 'var' to VAR.
		 * Features:
		 *
		 *  VAR?DEF    where DEF is a default value.
		 *
		 */

		a = s+1;	       
		b = strchr(a, '}');

		if (!b) {
			snprintf(errstr, errstr_size,
				 "Expecting '}' after \"%.*s...\"",
				 30, begin);
			return -1;
		}

		if (a == b) {
			snprintf(errstr, errstr_size,
				 "Empty {} identifier at \"%.*s...\"",
				 30, begin);
			return -1;
		}

		s = b+1;

		var = a;

		/* Check for ?DEF and !OPTIONs */
		if ((q = strnchrs(a, (int)(b-a), "?!"))) {
			const char *q2 = q;

			varlen = (int)(q - a);
			if (varlen == 0)
				var = NULL;

			/* Scan all ?DEF and !OPTIONs */
			do {
				int qlen;

				q++;

				if ((q2 = strnchrs(q, (int)(b-q2-1),
						   "@?!")))
					qlen = (int)(q2-q);
				else
					qlen = (int)(b-q);

				switch (*(q-1))
				{
				case '?':
					/* Default value */
					def = q;
					deflen = qlen;
					break;
				case '!':
					/* Options */
					if (0) {
					} else {
						snprintf(errstr,
							 errstr_size,
							 "Unknown "
							 "formatter "
							 "option "
							 "\"%.*s\" at "
							 "\"%.*s...\"",
							 qlen, q,
							 30, a);
						return -1;
					}
					break;
				}

			} while ((q = q2));

		} else
			varlen = (int)(b-a);			

		/* Add formatter to ordered list of formatters */
		if (!(fmt = format_add(fconf, var, varlen,
					def, deflen, flags,
					errstr, errstr_size)))
			return -1;

		cnt++;

		/* Now add a reference to the variable name, unless it is
		 * already found. */
		if (!(fmtvar = fmtvar_find(fconf, var, varlen)))
			fmtvar = fmtvar_add(fconf, var, varlen);

		fmt->varidx = fmtvar->idx;

		
		t = s = b+1;
	}

	/* "..%{..}....."
	 *         ^---^  add this part as verbatim string */
	if (s > t)
		if (!format_add(fconf, NULL, 0,
			       t, (int)(s - t), 0,
			       errstr, errstr_size))
			return -1;
	
	/* Add output delimiter to tail of format */
	format_add(fconf, NULL, 0,
		   conf.output_delimiter, strlen(conf.output_delimiter),
		   0, errstr, errstr_size);
						 

	/* Dump parsed format string. */
	if (conf.log_level >= 7)
		fmt_dump(fconf);


	if (fconf->fmt_cnt == 0) {
		snprintf(errstr, errstr_size,
			 "format string is empty");
		return -1;
	} else if (cnt == 0) {
		snprintf(errstr, errstr_size,
			 "No %%.. formatters in format");
		return -1;
	}

	return fconf->fmt_cnt;
}








