#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "houdini.h"
typedef struct _cmark_node_ cmark_node;
#include "exam.h"

/**
 * According to the OWASP rules:
 *
 * & --> &amp;
 * < --> &lt;
 * > --> &gt;
 * " --> &quot;
 * ' --> &#x27;     &apos; is not recommended
 * / --> &#x2F;     forward slash is included as it helps end an HTML entity
 *
 */
static const char HTML_ESCAPE_TABLE[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 1, 0, 0, 0, 2, 3, 0, 0, 0, 0, 0, 0, 0, 4,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 6, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const char *HTML_ESCAPES[] = {
	"",
	"&quot;",
	"&amp;",
	"&#39;",
	"&#47;",
	"&lt;",
	"&gt;"
};

// {blank(:[0-9]+} get translated to input boxes

int
houdini_escape_html0_but_blank(cmark_strbuf *ob, const uint8_t *src, size_t size, int secure)
{
	size_t  i = 0, org, esc = 0;

	while (i < size) {
		org = i;
		while (i < size && (esc = HTML_ESCAPE_TABLE[src[i]]) == 0)
			i++;

		//fprintf(stderr, "i:%d, org:%d %c  sl:%d, size:%d\n", (int)i, (int)org, (char)src[i], (int)i-(int)org, (int)size);
		if (i > org) {
		    // before we output, check for {blank...}
		    int scanlen = i-org;
		    int j;
		    int match = 0;
		    int val = 0;
		    int start = 0;
		    for (j=0; j<scanlen; j++) {
			//fprintf(stderr, "idx:%d j:%d, char:%c state:%d scanlen:%d\n", (int)j+(int)org, j, (char)src[org+j], match, scanlen);
			switch (match) {
			case 0:
			    if (((scanlen - j) > 6) && (strncmp((const char*)(src+org+j), "{blank", 6)==0)) {
				match = 1;
				start = j;
				j += 5;
				val = 0;
			    } 
			    break;

			case 1: switch (src[org+j]) {
			    case ' ': break;
			    case ':':
				match = 2; break;
			    case '}':
				match = 3; break;
			    default:
				match = 0;
			    }
			    break;

			case 2:
			    switch (src[org+j]) {
			    case ' ': break;
			    case '}': match = 3; break;
			    case '0':
			    case '1':
			    case '2':
			    case '3':
			    case '4':
			    case '5':
			    case '6':
			    case '7':
			    case '8':
			    case '9':
				val = (val*10)+(src[org+j]-'0');
				break;
			    default:
				match = 0;
			    }
			}
			if (match == 3) {
			    // we found a match
			    //fprintf(stderr, "FOUND MATCH:%d  start:%d\n", (int)org, start);
			    if (start > 0) {
				cmark_strbuf_put(ob, src + org, start);
			    }
			    if (val == 0) val = 10;
			    format_text_wiget(ob, val, -1, -1);
			    org += (j+1);
			    //fprintf(stderr, "Will start from: %d [%c]\n", (int)org, src[org]);
			    scanlen -= (j+1);
			    j = 0;
			    match = 0;
			}
		    }
		    //fprintf(stderr, "Now out rest: i:%d, org:%d\n", (int)i, (int)org);
		    if (i>org) {
			cmark_strbuf_put(ob, src + org, i - org);
		    }
		}

		/* escaping */
		if (unlikely(i >= size))
			break;

		/* The forward slash is only escaped in secure mode */
		if ((src[i] == '/' || src[i] == '\'') && !secure) {
			cmark_strbuf_putc(ob, src[i]);
		} else {
			cmark_strbuf_puts(ob, HTML_ESCAPES[esc]);
		}

		i++;
	}

	return 1;
}

int 
houdini_escape_html0(cmark_strbuf *ob, const uint8_t *src, size_t size, int secure)
{
    size_t  i = 0, org, esc = 0;

    while (i < size) {
	org = i;
	while (i < size && (esc = HTML_ESCAPE_TABLE[src[i]]) == 0)
	    i++;

	if (i > org)
	    cmark_strbuf_put(ob, src + org, i - org);

	/* escaping */
	if (unlikely(i >= size))
	    break;

	/* The forward slash is only escaped in secure mode */
	if ((src[i] == '/' || src[i] == '\'') && !secure) {
	    cmark_strbuf_putc(ob, src[i]);
	} else {
	    cmark_strbuf_puts(ob, HTML_ESCAPES[esc]);
	}

	i++;
    }

    return 1;
}

int
houdini_escape_html(cmark_strbuf *ob, const uint8_t *src, size_t size)
{
	return houdini_escape_html0(ob, src, size, 1);
}


// Local Variables:
// mode: c           
// c-basic-offset: 4
// End:
