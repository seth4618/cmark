#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "cmark_ctype.h"
#include "config.h"
#include "node.h"
#include "parser.h"
#include "references.h"
#include "cmark.h"
#include "houdini.h"
#include "utf8.h"
#include "scanners.h"
#include "inlines.h"

extern int cmark_verbose;

void failed2match(const char* text, const char* reason);

static cmark_node* 
find_closest_node_with_line_info(cmark_node* node)
{
    if (node == NULL) return NULL;
    while ((node->start_line == 0) && (node->prev != NULL)) { node=node->prev; }
    if (node->start_line != 0) return node;
    return find_closest_node_with_line_info(node->parent);
}

static int
find_closest_line(cmark_node* node)
{
    return cmark_node_get_start_line(find_closest_node_with_line_info(node));
}

static int
find_closest_column(cmark_node* node)
{
    return cmark_node_get_start_column(find_closest_node_with_line_info(node));
}

static char*
remnl(const char* x)
{
    static char buffer[1024];
    char* p = buffer;
    while (*x) *p++ = *x++;
    if (*(p-1) == '\n') p--;
    *p = 0;
    return buffer;
}

void 
cmark_chunk_show(cmark_chunk* c)
{
    fprintf(stderr, "{[%s], %d, %d}\n", c->data, c->len, c->alloc);
}



static const char *EMDASH = "\xE2\x80\x94";
static const char *ENDASH = "\xE2\x80\x93";
static const char *ELLIPSES = "\xE2\x80\xA6";
static const char *LEFTDOUBLEQUOTE = "\xE2\x80\x9C";
static const char *RIGHTDOUBLEQUOTE = "\xE2\x80\x9D";
static const char *LEFTSINGLEQUOTE = "\xE2\x80\x98";
static const char *RIGHTSINGLEQUOTE = "\xE2\x80\x99";


// Macros for creating various kinds of simple.
#define make_str(s) make_literal(CMARK_NODE_TEXT, s)
#define make_code(s) make_literal(CMARK_NODE_CODE, s)
#define make_raw_html(s) make_literal(CMARK_NODE_INLINE_HTML, s)
#define make_linebreak() make_simple(CMARK_NODE_LINEBREAK)
#define make_softbreak() make_simple(CMARK_NODE_SOFTBREAK)
#define make_emph() make_simple(CMARK_NODE_EMPH)
#define make_strong() make_simple(CMARK_NODE_STRONG)

#define DEBUG

typedef struct delimiter {
	struct delimiter *previous;
	struct delimiter *next;
	cmark_node *inl_text;
	int position;
	unsigned char delim_char;
	bool can_open;
	bool can_close;
	bool active;
} delimiter;

typedef struct {
	cmark_chunk input;
	int pos;
	cmark_reference_map *refmap;
	delimiter *last_delim;
} subject;

static delimiter*
S_insert_emph(subject *subj, delimiter *opener, delimiter *closer);

static int parse_inline(subject* subj, cmark_node * parent, int options);

static void subject_from_buf(subject *e, cmark_strbuf *buffer,
                             cmark_reference_map *refmap);
static int subject_find_special_char(subject *subj, int options);

static cmark_chunk cmark_clean_autolink(cmark_chunk *url, int is_email)
{
    cmark_strbuf buf = GH_BUF_INIT;

    cmark_chunk_trim(url);

    if (url->len == 0) {
	cmark_chunk result = CMARK_CHUNK_EMPTY;
	return result;
    }

    if (is_email)
	cmark_strbuf_puts(&buf, "mailto:");

    houdini_unescape_html_f(&buf, url->data, url->len);
    return cmark_chunk_buf_detach(&buf);
}

static inline cmark_node *make_link(cmark_node *label, cmark_chunk *url, cmark_chunk *title)
{
    cmark_node* e = newNode(CMARK_NODE_LINK);

    e->first_child   = label;
    e->last_child    = label;
    e->as.link.url   = *url;
    e->as.link.title = *title;
    e->next = NULL;
    label->parent = e;
  
    return e;
}

static inline cmark_node* 
make_autolink(cmark_node* label, cmark_chunk url, int is_email)
{
    cmark_chunk clean_url = cmark_clean_autolink(&url, is_email);
    cmark_chunk title = CMARK_CHUNK_EMPTY;
    return make_link(label, &clean_url, &title);
}

// Create an inline with a literal string value.
static inline cmark_node* 
make_literal(cmark_node_type t, cmark_chunk s)
{
    cmark_node * e = newNode(t);

    e->as.literal = s;
    e->next = NULL;
    e->prev = NULL;
    e->parent = NULL;
    e->first_child = NULL;
    e->last_child = NULL;
    // These fields aren't used for inlines:
    e->start_line = 0;
    e->start_column = 0;
    e->end_line = 0;

    return e;
}

// Create an inline with no value.
static inline cmark_node* make_simple(cmark_node_type t)
{
    cmark_node* e = newNode(t);

    e->next = NULL;
    e->prev = NULL;
    e->parent = NULL;
    e->first_child = NULL;
    e->last_child = NULL;
    // These fields aren't used for inlines:
    e->start_line = 0;
    e->start_column = 0;
    e->end_line = 0;

    return e;
}

// Duplicate a chunk by creating a copy of the buffer not by reusing the
// buffer like cmark_chunk_dup does.
static cmark_chunk chunk_clone(cmark_chunk *src)
{
    cmark_chunk c;
    int len = src->len;

    c.len   = len;
    c.data  = (unsigned char *)malloc(len + 1);
    c.alloc = 1;
    memcpy(c.data, src->data, len);
    c.data[len] = '\0';

    return c;
}

static void subject_from_buf(subject *e, cmark_strbuf *buffer,cmark_reference_map *refmap)
{
    e->input.data = buffer->ptr;
    e->input.len = buffer->size;
    e->input.alloc = 0;
    e->pos = 0;
    e->refmap = refmap;
    e->last_delim = NULL;

    cmark_chunk_rtrim(&e->input);
}

static inline int isbacktick(int c)
{
    return (c == '`');
}

static inline unsigned char peek_char(subject *subj)
{
    return (subj->pos < subj->input.len) ? subj->input.data[subj->pos] : 0;
}

static inline unsigned char peek_at(subject *subj, int pos)
{
    return subj->input.data[pos];
}

#if 0
static unsigned char 
peek_string(subject* subj, const char* qtext)
{
    int qlen = strlen(qtext);
    int pos = subj->pos;
    if (subj->input.len < pos+qlen) return 0;
    int i;
    for (i=0; i<qlen; i++) if (peek_at(subj, pos+i) != qtext[i]) return 0;
    return 1;
}

static unsigned char peek_question(subject* subj)
{
    return peek_string(subj, "question"); 
}
#endif

// Return true if there are more characters in the subject.
static inline int is_eof(subject* subj)
{
	return (subj->pos >= subj->input.len);
}

// Advance the subject.  Doesn't check for eof.
#define advance(subj) (subj)->pos += 1

// Take characters while a predicate holds, and return a string.
static inline cmark_chunk take_while(subject* subj, int (*f)(int))
{
    unsigned char c;
    int startpos = subj->pos;
    int len = 0;

    while ((c = peek_char(subj)) && (*f)(c)) {
	advance(subj);
	len++;
    }

    return cmark_chunk_dup(&subj->input, startpos, len);
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the subject
// after the closing backticks.
static int scan_to_closing_backticks(subject* subj, int openticklength)
{
    // read non backticks
    unsigned char c;
    while ((c = peek_char(subj)) && c != '`') {
	advance(subj);
    }
    if (is_eof(subj)) {
	return 0;  // did not find closing ticks, return 0
    }
    int numticks = 0;
    while (peek_char(subj) == '`') {
	advance(subj);
	numticks++;
    }
    if (numticks != openticklength) {
	return(scan_to_closing_backticks(subj, openticklength));
    }
    return (subj->pos);
}

// Parse backtick code section or raw backticks, return an inline.
// Assumes that the subject has a backtick at the current position.
static cmark_node* handle_backticks(subject *subj)
{
    cmark_chunk openticks = take_while(subj, isbacktick);
    int startpos = subj->pos;
    int endpos = scan_to_closing_backticks(subj, openticks.len);

    if (endpos == 0) { // not found
	subj->pos = startpos; // rewind
	return make_str(openticks);
    } else {
	cmark_strbuf buf = GH_BUF_INIT;

	cmark_strbuf_set(&buf, subj->input.data + startpos, endpos - startpos - openticks.len);
	cmark_strbuf_trim(&buf);
	cmark_strbuf_normalize_whitespace(&buf);

	return make_code(cmark_chunk_buf_detach(&buf));
    }
}

// Scan ***, **, or * and return number scanned, or 0.
// Advances position.
static int
scan_delims(subject* subj, unsigned char c, bool * can_open, bool * can_close)
{
    int numdelims = 0;
    int before_char_pos;
    int32_t after_char = 0;
    int32_t before_char = 0;
    int len;
    bool left_flanking, right_flanking;

    if (subj->pos == 0) {
	before_char = 10;
    } else {
	before_char_pos = subj->pos - 1;
	// walk back to the beginning of the UTF_8 sequence:
	while (peek_at(subj, before_char_pos) >> 6 == 2 &&
	       before_char_pos > 0) {
	    before_char_pos -= 1;
	}
	len = utf8proc_iterate(subj->input.data + before_char_pos,
			       subj->pos - before_char_pos, &before_char);
	if (len == -1) {
	    before_char = 10;
	}
    }

    if (c == '\'' || c == '"') {
	numdelims++;
	advance(subj);  // limit to 1 delim for quotes
    } else {
	while (peek_char(subj) == c) {
	    numdelims++;
	    advance(subj);
	}
    }

    len = utf8proc_iterate(subj->input.data + subj->pos,
			   subj->input.len - subj->pos, &after_char);
    if (len == -1) {
	after_char = 10;
    }
    left_flanking = numdelims > 0 && !utf8proc_is_space(after_char) &&
	!(utf8proc_is_punctuation(after_char) &&
	  !utf8proc_is_space(before_char) &&
	  !utf8proc_is_punctuation(before_char));
    right_flanking = numdelims > 0 && !utf8proc_is_space(before_char) &&
	!(utf8proc_is_punctuation(before_char) &&
	  !utf8proc_is_space(after_char) &&
	  !utf8proc_is_punctuation(after_char));
    if (c == '_') {
	*can_open = left_flanking &&
	    (!right_flanking || utf8proc_is_punctuation(before_char));
	*can_close = right_flanking &&
	    (!left_flanking || utf8proc_is_punctuation(after_char));
    } else if (c == '\'' || c == '"') {
	*can_open = left_flanking && !right_flanking;
	*can_close = right_flanking;
    } else {
	*can_open = left_flanking;
	*can_close = right_flanking;
    }
    return numdelims;
}

/*
static void print_delimiters(subject *subj)
{
	delimiter *delim;
	delim = subj->last_delim;
	while (delim != NULL) {
		printf("Item at %p: %d %d %d next(%p) prev(%p)\n",
		       delim, delim->delim_char,
		       delim->can_open, delim->can_close,
		       delim->next, delim->previous);
		delim = delim->previous;
	}
}
*/

static void remove_delimiter(subject *subj, delimiter *delim)
{
    if (delim == NULL) return;
    if (delim->next == NULL) {
	// end of list:
	assert(delim == subj->last_delim);
	subj->last_delim = delim->previous;
    } else {
	delim->next->previous = delim->previous;
    }
    if (delim->previous != NULL) {
	delim->previous->next = delim->next;
    }
    free(delim);
}

static void push_delimiter(subject *subj, unsigned char c, bool can_open,
                           bool can_close, cmark_node *inl_text)
{
    delimiter *delim =
	(delimiter*)malloc(sizeof(delimiter));
    if (delim == NULL) {
	return;
    }
    delim->delim_char = c;
    delim->can_open = can_open;
    delim->can_close = can_close;
    delim->inl_text = inl_text;
    delim->previous = subj->last_delim;
    delim->next = NULL;
    if (delim->previous != NULL) {
	delim->previous->next = delim;
    }
    delim->position = subj->pos;
    delim->active = true;
    subj->last_delim = delim;
}

// Assumes the subject has a c at the current position.
static cmark_node* handle_delim(subject* subj, unsigned char c, bool smart)
{
    int numdelims;
    cmark_node * inl_text;
    bool can_open, can_close;
    cmark_chunk contents;

    numdelims = scan_delims(subj, c, &can_open, &can_close);

    if (c == '\'' && smart) {
	contents = cmark_chunk_literal(RIGHTSINGLEQUOTE);
    } else if (c == '"' && smart) {
	contents = cmark_chunk_literal(can_close ? RIGHTDOUBLEQUOTE : LEFTDOUBLEQUOTE);
    } else {
	contents = cmark_chunk_dup(&subj->input, subj->pos - numdelims, numdelims);
    }

    inl_text = make_str(contents);

    if ((can_open || can_close) &&
	(!(c == '\'' || c == '"') || smart)) {
	push_delimiter(subj, c, can_open, can_close, inl_text);
    }

    return inl_text;
}

// Assumes we have a hyphen at the current position.
static cmark_node* handle_hyphen(subject* subj, bool smart)
{
    advance(subj);
    if (smart && peek_char(subj) == '-') {
	advance(subj);
	if (peek_char(subj) == '-') {
	    advance(subj);
	    return make_str(cmark_chunk_literal(EMDASH));
	} else {
	    return make_str(cmark_chunk_literal(ENDASH));
	}
    } else {
	return make_str(cmark_chunk_literal("-"));
    }
}

// Assumes we have a period at the current position.
static cmark_node* handle_period(subject* subj, bool smart)
{
    advance(subj);
    if (smart && peek_char(subj) == '.') {
	advance(subj);
	if (peek_char(subj) == '.') {
	    advance(subj);
	    return make_str(cmark_chunk_literal(ELLIPSES));
	} else {
	    return make_str(cmark_chunk_literal(".."));
	}
    } else {
	return make_str(cmark_chunk_literal("."));
    }
}

static void process_emphasis(subject *subj, delimiter *start_delim)
{
    delimiter *closer = subj->last_delim;
    delimiter *opener;

    // move back to first relevant delim.
    while (closer != NULL && closer->previous != start_delim) {
	closer = closer->previous;
    }

    // now move forward, looking for closers, and handling each
    while (closer != NULL) {
	if (closer->can_close &&
	    (closer->delim_char == '*' || closer->delim_char == '_' ||
	     closer->delim_char == '"' || closer->delim_char == '\'')) {
	    // Now look backwards for first matching opener:
	    opener = closer->previous;
	    while (opener != NULL && opener != start_delim) {
		if (opener->delim_char == closer->delim_char &&
		    opener->can_open) {
		    break;
		}
		opener = opener->previous;
	    }
	    if (closer->delim_char == '*' || closer->delim_char == '_') {
		if (opener != NULL && opener != start_delim) {
		    closer = S_insert_emph(subj, opener, closer);
		} else {
		    closer = closer->next;
		}
	    } else if (closer->delim_char == '\'') {
		cmark_chunk_free(&closer->inl_text->as.literal);
		closer->inl_text->as.literal =
		    cmark_chunk_literal(RIGHTSINGLEQUOTE);
		if (opener != NULL && opener != start_delim) {
		    cmark_chunk_free(&opener->inl_text->as.literal);
		    opener->inl_text->as.literal =
			cmark_chunk_literal(LEFTSINGLEQUOTE);
		}
		closer = closer->next;
	    } else if (closer->delim_char == '"') {
		cmark_chunk_free(&closer->inl_text->as.literal);
		closer->inl_text->as.literal =
		    cmark_chunk_literal(RIGHTDOUBLEQUOTE);
		if (opener != NULL && opener != start_delim) {
		    cmark_chunk_free(&opener->inl_text->as.literal);
		    opener->inl_text->as.literal =
			cmark_chunk_literal(LEFTDOUBLEQUOTE);
		}
		closer = closer->next;
	    }
	} else {
	    closer = closer->next;
	}
    }
    // free all delimiters in list until start_delim:
    while (subj->last_delim != start_delim) {
	remove_delimiter(subj, subj->last_delim);
    }
}

static delimiter*
S_insert_emph(subject *subj, delimiter *opener, delimiter *closer)
{
    delimiter *delim, *tmp_delim;
    int use_delims;
    cmark_node *opener_inl = opener->inl_text;
    cmark_node *closer_inl = closer->inl_text;
    int opener_num_chars = opener_inl->as.literal.len;
    int closer_num_chars = closer_inl->as.literal.len;
    cmark_node *tmp, *emph, *first_child, *last_child;

    // calculate the actual number of characters used from this closer
    if (closer_num_chars < 3 || opener_num_chars < 3) {
	use_delims = closer_num_chars <= opener_num_chars ?
	    closer_num_chars : opener_num_chars;
    } else { // closer and opener both have >= 3 characters
	use_delims = closer_num_chars % 2 == 0 ? 2 : 1;
    }

    // remove used characters from associated inlines.
    opener_num_chars -= use_delims;
    closer_num_chars -= use_delims;
    opener_inl->as.literal.len = opener_num_chars;
    closer_inl->as.literal.len = closer_num_chars;

    // free delimiters between opener and closer
    delim = closer->previous;
    while (delim != NULL && delim != opener) {
	tmp_delim = delim->previous;
	remove_delimiter(subj, delim);
	delim = tmp_delim;
    }

    first_child = opener_inl->next;
    last_child  = closer_inl->prev;

    // if opener has 0 characters, remove it and its associated inline
    if (opener_num_chars == 0) {
	// replace empty opener inline with emph
	cmark_chunk_free(&(opener_inl->as.literal));
	emph = opener_inl;
	emph->type = use_delims == 1 ? NODE_EMPH : NODE_STRONG;
	// remove opener from list
	remove_delimiter(subj, opener);
    } else {
	// create new emph or strong, and splice it in to our inlines
	// between the opener and closer
	emph = use_delims == 1 ? make_emph() : make_strong();
	emph->parent = opener_inl->parent;
	emph->prev = opener_inl;
	opener_inl->next = emph;
    }

    // push children below emph
    emph->next = closer_inl;
    closer_inl->prev = emph;
    emph->first_child = first_child;
    emph->last_child  = last_child;

    // fix children pointers
    first_child->prev = NULL;
    last_child->next  = NULL;
    for (tmp = first_child; tmp != NULL; tmp = tmp->next) {
	tmp->parent = emph;
    }

    // if closer has 0 characters, remove it and its associated inline
    if (closer_num_chars == 0) {
	// remove empty closer inline
	cmark_node_free(closer_inl);
	// remove closer from list
	tmp_delim = closer->next;
	remove_delimiter(subj, closer);
	closer = tmp_delim;
    }

    return closer;
}

// Parse backslash-escape or just a backslash, returning an inline.
static cmark_node* handle_backslash(subject *subj)
{
    advance(subj);
    unsigned char nextchar = peek_char(subj);
    if (cmark_ispunct(nextchar)) {  // only ascii symbols and newline can be escaped
	advance(subj);
	return make_str(cmark_chunk_dup(&subj->input, subj->pos - 1, 1));
    } else if (nextchar == '\n') {
	advance(subj);
	return make_linebreak();
    } else {
	return make_str(cmark_chunk_literal("\\"));
    }
}

// Parse an entity or a regular "&" string.
// Assumes the subject has an '&' character at the current position.
static cmark_node* handle_entity(subject* subj)
{
    cmark_strbuf ent = GH_BUF_INIT;
    size_t len;

    advance(subj);

    len = houdini_unescape_ent(&ent,
			       subj->input.data + subj->pos,
			       subj->input.len - subj->pos
			       );

    if (len == 0)
	return make_str(cmark_chunk_literal("&"));

    subj->pos += len;
    return make_str(cmark_chunk_buf_detach(&ent));
}

// Like make_str, but parses entities.
// Returns an inline sequence consisting of str and entity elements.
static cmark_node *make_str_with_entities(cmark_chunk *content)
{
	cmark_strbuf unescaped = GH_BUF_INIT;

	if (houdini_unescape_html(&unescaped, content->data, (size_t)content->len)) {
		return make_str(cmark_chunk_buf_detach(&unescaped));
	} else {
		return make_str(*content);
	}
}

// Clean a URL: remove surrounding whitespace and surrounding <>,
// and remove \ that escape punctuation.
cmark_chunk cmark_clean_url(cmark_chunk *url)
{
	cmark_strbuf buf = GH_BUF_INIT;

	cmark_chunk_trim(url);

	if (url->len == 0) {
		cmark_chunk result = CMARK_CHUNK_EMPTY;
		return result;
	}

	if (url->data[0] == '<' && url->data[url->len - 1] == '>') {
		houdini_unescape_html_f(&buf, url->data + 1, url->len - 2);
	} else {
		houdini_unescape_html_f(&buf, url->data, url->len);
	}

	cmark_strbuf_unescape(&buf);
	return cmark_chunk_buf_detach(&buf);
}

cmark_chunk cmark_clean_title(cmark_chunk *title)
{
	cmark_strbuf buf = GH_BUF_INIT;
	unsigned char first, last;

	if (title->len == 0) {
		cmark_chunk result = CMARK_CHUNK_EMPTY;
		return result;
	}

	first = title->data[0];
	last = title->data[title->len - 1];

	// remove surrounding quotes if any:
	if ((first == '\'' && last == '\'') ||
	    (first == '(' && last == ')') ||
	    (first == '"' && last == '"')) {
		houdini_unescape_html_f(&buf, title->data + 1, title->len - 2);
	} else {
		houdini_unescape_html_f(&buf, title->data, title->len);
	}

	cmark_strbuf_unescape(&buf);
	return cmark_chunk_buf_detach(&buf);
}

// Parse an autolink or HTML tag.
// Assumes the subject has a '<' character at the current position.
static cmark_node* handle_pointy_brace(subject* subj)
{
    int matchlen = 0;
    int inline_match = 0;
    int sps = 0;
    cmark_chunk contents;

    advance(subj);  // advance past first <

    // first try to match a URL autolink
    matchlen = scan_autolink_uri(&subj->input,subj->pos);
    if(matchlen==0)
	{
	    sps = scan_spacechars(&subj->input,subj->pos);
	    subj->pos+=sps;
	    inline_match = scan_autolink_inline(&subj->input,subj->pos);
	    if(inline_match)
		{
		    sps = scan_spacechars(&(subj->input),subj->pos+inline_match);
		}
	}
    if (matchlen > 0) {
	contents = cmark_chunk_dup(&subj->input, subj->pos, matchlen - 1);
	subj->pos += matchlen;

	return make_autolink(make_str(contents),contents, 0);
    }
    else if(inline_match>0)
	{
	    int temp = subj->pos;
	    subj->pos+=inline_match+sps;
	    if(peek_char(subj)!='>')
		{
		    return make_str(cmark_chunk_literal("<"));
		}
	    //now we know that we have a valid reference to an inline link because it ended with >
	    subj->pos = temp;
	    cmark_chunk label = cmark_chunk_dup(      &subj->input,subj->pos+1,inline_match+sps-1);
	    contents = cmark_chunk_dup(&subj->input, subj->pos,inline_match);
	    subj->pos +=inline_match+sps;

	    //advance subject so that you don't add the > at the end of the link
	    advance(subj);
	    return make_autolink(make_str(label),contents,0);
	}

    // next try to match an email autolink
    matchlen = scan_autolink_email(&subj->input, subj->pos);
    if (matchlen > 0) {
	contents = cmark_chunk_dup(&subj->input, subj->pos, matchlen - 1);
	subj->pos += matchlen;

	return make_autolink(
			     make_str_with_entities(&contents),
			     contents, 1
			     );
    }

    // finally, try to match an html tag
    matchlen = scan_html_tag(&subj->input, subj->pos);
    if (matchlen > 0) {
	contents = cmark_chunk_dup(&subj->input, subj->pos - 1, matchlen + 1);
	subj->pos += matchlen;
	return make_raw_html(contents);
    }

    // if nothing matches, just return the opening <:
    return make_str(cmark_chunk_literal("<"));
}

// Parse a link label.  Returns 1 if successful.
// Note:  unescaped brackets are not allowed in labels.
// The label begins with `[` and ends with the first `]` character
// encountered.  Backticks in labels do not start code spans.
static int link_label(subject* subj, cmark_chunk *raw_label)
{
    int startpos = subj->pos;
    int length = 0;
    unsigned char c;

    // advance past [
    if (peek_char(subj) == '[') {
	advance(subj);
    } else {
	return 0;
    }

    while ((c = peek_char(subj)) && c != '[' && c != ']') {
	if (c == '\\') {
	    advance(subj);
	    length++;
	    if (cmark_ispunct(peek_char(subj))) {
		advance(subj);
		length++;
	    }
	} else {
	    advance(subj);
	    length++;
	}
	if (length > MAX_LINK_LABEL_LENGTH) {
	    goto noMatch;
	}
    }

    if (c == ']') { // match found
	*raw_label = cmark_chunk_dup(&subj->input, startpos + 1, subj->pos - (startpos + 1));
	advance(subj);  // advance past ]
	return 1;
    }

 noMatch:
    subj->pos = startpos; // rewind
    return 0;

}

// On encountering a } this function processes:
// - inline links
// - question markers
// - question blanks

static cmark_node* 
handle_close_curly_brace(subject* subj,cmark_node* parent)
{
    delimiter *opener = subj->last_delim;
    cmark_node *inl;

    if (cmark_verbose) fprintf(stderr, "============== %d in %d[%s] %p\n", subj->pos, subj->input.len, remnl((char*)subj->input.data), (void*)parent);

    advance(subj); //advance past }
    while(opener) {
	if(opener->delim_char=='{') {
	    break;
	}
	opener = opener->previous;
    }
    if(opener == NULL) {
	return make_str(cmark_chunk_literal("}"));
    }

    if (cmark_verbose) fprintf(stderr, "Not Null\n");

    if(!opener->active) {
	remove_delimiter(subj,opener);
	return make_str(cmark_chunk_literal("}"));
    }

    if (cmark_verbose)  {
	cmark_node* rest = opener->inl_text->next;
	while (rest) {
	    fprintf(stderr, "[%s] -> ", cmark_chunk_to_cstr(&rest->as.literal));
	    rest = rest->next;
	}
	fprintf(stderr, "(null)\n");
    }

    char* remaining = "";
    int rlen = 0;
    cmark_node* rest = opener->inl_text->next;
    if (opener->inl_text->next != NULL) {
	// count length of string
	int len = 0;
	for (cmark_node* node = opener->inl_text->next; node; node=node->next) {
	    len += node->as.literal.len;
	}
	remaining = calloc(len+1, sizeof(char));
	len = 0;
	for (cmark_node* node = opener->inl_text->next; node; node=node->next) {
	    strncpy(remaining+len, cmark_chunk_to_cstr(&node->as.literal), node->as.literal.len);
	    len += node->as.literal.len;
	}
	remaining[len] = 0;
	const char* p = remaining;
	while (*p && (*p != '}')) p++;
	rlen = p-remaining;
	if (cmark_verbose) fprintf(stderr, "and rest = [%s] %d\n", remnl(remaining), rlen);
    }

    if (cmark_verbose) fprintf(stderr, "remaining: [%s]\n", remaining);
    if (remaining[0] == '#') {
	//came here so have a full inline link
	inl = opener->inl_text;
	inl->type = NODE_INLINE_LINK;
	cmark_node* tag_name = opener->inl_text->next;
	cmark_chunk_free(&inl->as.literal);
	const char* realtag = cmark_node_get_literal(tag_name);
	if(!cmark_node_set_literal(inl,realtag+1))  {
	    fprintf(stderr,"Couldn't set literal. Exiting! \n");
	    exit(1);
	}
	cmark_node_free(tag_name);
	return NULL;
    } else if (remaining[0] == ':') {
	// this is an attribute to be attached to the block this is touching (or contained in)
	inl = opener->inl_text;
	inl->type = NODE_ATTRIBUTE;
	cmark_chunk_free(&inl->as.literal);
	inl->as.literal = cmark_chunk_literal((const char*)remaining+1);
	if (cmark_verbose) fprintf(stderr, "Attribute: [%s]\n", cmark_chunk_to_cstr(&inl->as.literal));
	cmark_tree_free(rest);
	return NULL;
    } else if ((rlen >= 8) && (strncmp((const char*)remaining, "question", 8) == 0)) {
	// we have a question node
	inl = opener->inl_text;
	inl->type = NODE_QUEST;
	inl->start_line = find_closest_line(inl);
	inl->start_column = find_closest_column(inl)+subj->pos-8;
	//fprintf(stderr, "Found question at %d:%d\n", inl->start_line, inl->start_column);
	int level = 1;
	remaining += 8;
	rlen -= 8;
	if (rlen > 0) {
	    // look at rest of text
	    if (remaining[0] != ':') {
		// fatal error
		failed2match("':' after question", (const char*)remaining);
	    }
	    level = remaining[1]-'0';
	}
	cmark_chunk_free(&inl->as.literal);
	inl->user_data = malloc(sizeof(char)*4);
	sprintf(inl->user_data,"%d",level);
	inl->as.qlevel = level;
	cmark_node_free(rest);
	return NULL;
    } else if ((rlen >= 6) && (strncmp((const char*)remaining, "answer", 6) == 0)) {
	// we have an answer node
	inl = opener->inl_text;
	inl->type = NODE_ANSWER;
	remaining += 6;
	rlen -= 6;
	if (rlen > 0) {
	    // there is some other stuff there
	    if (remaining[0] != ':') {
		// fatal error
		failed2match("':' after answer", (const char*)remaining);
	    }
	    inl->user_data = (char *)calloc(1, rlen);
	    strncpy(inl->user_data, (const char*)remaining+1, rlen-1);
	} else {
	    char* x = (char *)calloc(1, 1);
	    x[0] = 0;
	    inl->user_data = x;
	}
	cmark_chunk_free(&inl->as.literal);
	cmark_node_free(rest);
	if (cmark_verbose) fprintf(stderr, "Found {answer:%s}\n", remnl((char*)inl->user_data));
	return NULL;
    } else if ((rlen >= 5) && (strncmp((const char*)remaining, "blank", 5) == 0)) {
	inl = opener->inl_text;
	inl->type = NODE_BLANK;
	remaining += 5;
	rlen -= 5;
	if (cmark_verbose) fprintf(stderr, "%d, [%s]\n", rlen, remnl((char*)remaining));
	if (rlen > 0) {
	    // there is some other stuff there
	    if (remaining[0] != ':') {
		// fatal error
		failed2match("':' after blank", (const char*)remaining);
	    }
	    inl->user_data = (char *)calloc(1, rlen);
	    strncpy(inl->user_data, (const char*)remaining+1, rlen-1);
	} else {
	    char* x = (char *)calloc(1, 1);
	    x[0] = 0;
	    inl->user_data = x;
	}
	cmark_chunk_free(&inl->as.literal);
	cmark_node_free(rest);
	if (cmark_verbose) fprintf(stderr, "Found {blank:%s}\n", remnl((char*)inl->user_data));
	return NULL;
    }
    // This is not a question or an answer
    if (cmark_verbose) fprintf(stderr, "Found {random}\n");
    remove_delimiter(subj,opener);
    return make_str(cmark_chunk_literal("}"));
}

// Return a link, an image, or a literal close bracket.
static cmark_node* handle_close_bracket(subject* subj, cmark_node *parent)
{
    int initial_pos;
    int starturl, endurl, starttitle, endtitle, endall;
    int n;
    int sps;
    cmark_reference *ref;
    bool is_image = false;
    cmark_chunk url_chunk, title_chunk;
    cmark_chunk url, title;
    delimiter *opener;
    cmark_node *link_text;
    cmark_node *inl;
    cmark_chunk raw_label;
    int found_label;

    advance(subj);  // advance past ]
    initial_pos = subj->pos;

    // look through list of delimiters for a [ or !
    opener = subj->last_delim;
    while (opener) {
	if (opener->delim_char == '[' || opener->delim_char == '!') {
	    break;
	}
	opener = opener->previous;
    }

    if (opener == NULL) {
	return make_str(cmark_chunk_literal("]"));
    }

    if (!opener->active) {
	// take delimiter off stack
        //could happen if you had 2 [[
	remove_delimiter(subj, opener);
	return make_str(cmark_chunk_literal("]"));
    }

    // If we got here, we matched a potential link/image text.
    is_image = opener->delim_char == '!';
    link_text = opener->inl_text->next;

    // Now we check to see if it's a link/image.

    // First, look for an inline link.
    if (peek_char(subj) == '(' &&
	((sps = scan_spacechars(&subj->input, subj->pos + 1)) > -1) &&
	((n = scan_link_url(&subj->input, subj->pos + 1 + sps)) > -1)) {

	// try to parse an explicit link:
	starturl = subj->pos + 1 + sps; // after ( including spaces
	endurl = starturl + n;
        //optional title parsing
	starttitle = endurl + scan_spacechars(&subj->input, endurl);

	// ensure there are spaces btw url and title
	endtitle = (starttitle == endurl) ? starttitle :
	    starttitle + scan_link_title(&subj->input, starttitle);

	endall = endtitle + scan_spacechars(&subj->input, endtitle);

	if (peek_at(subj, endall) == ')') {
	    subj->pos = endall + 1;

	    url_chunk = cmark_chunk_dup(&subj->input, starturl, endurl - starturl);
	    title_chunk = cmark_chunk_dup(&subj->input, starttitle, endtitle - starttitle);
	    url = cmark_clean_url(&url_chunk);
	    title = cmark_clean_title(&title_chunk);
	    cmark_chunk_free(&url_chunk);
	    cmark_chunk_free(&title_chunk);
	    goto match;

	} else {
	    goto noMatch;
	}
    }

    // Next, look for a following [link label] that matches in refmap.
    // skip spaces
    subj->pos = subj->pos + scan_spacechars(&subj->input, subj->pos);
    raw_label = cmark_chunk_literal("");
    found_label = link_label(subj, &raw_label);
    if (!found_label || raw_label.len == 0) {
	cmark_chunk_free(&raw_label);
	raw_label = cmark_chunk_dup(&subj->input, opener->position,initial_pos - opener->position - 1);
    }

    if (!found_label) {
	// If we have a shortcut reference link, back up
	// to before the spaces we skipped.
	subj->pos = initial_pos;
    }
    ref = cmark_reference_lookup(subj->refmap, &raw_label);
    cmark_chunk_free(&raw_label);

    if (ref != NULL) { // found
	url   = chunk_clone(&ref->url);
	title = chunk_clone(&ref->title);
	goto match;
    } else {
	goto noMatch;
    }

 noMatch:
    // If we fall through to here, it means we didn't match a link:
    remove_delimiter(subj, opener);  // remove this opener from delimiter list
    subj->pos = initial_pos;
    return make_str(cmark_chunk_literal("]"));

 match:
    inl = opener->inl_text;
    inl->type = is_image ? NODE_IMAGE : NODE_LINK;
    cmark_chunk_free(&inl->as.literal);
    inl->first_child = link_text;
    process_emphasis(subj, opener->previous);
    inl->as.link.url   = url;
    inl->as.link.title = title;
    inl->next = NULL;
    if (link_text) {
	cmark_node *tmp;
	link_text->prev = NULL;
	for (tmp = link_text; tmp->next != NULL; tmp = tmp->next) {
	    tmp->parent = inl;
	}
	tmp->parent = inl;
	inl->last_child = tmp;
    }
    parent->last_child = inl;

    // process_emphasis will remove this delimiter and all later ones.
    // Now, if we have a link, we also want to deactivate earlier link
    // delimiters. (This code can be removed if we decide to allow links
    // inside links.)
    if (!is_image) {
	opener = subj->last_delim;
	while (opener != NULL) {
	    if (opener->delim_char == '[') {
		if (!opener->active) {
		    break;
		} else {
		    opener->active = false;
		}
	    }
	    opener = opener->previous;
	}
    }

    return NULL;
}

// Parse a hard or soft linebreak, returning an inline.
// Assumes the subject has a newline at the current position.
static cmark_node* handle_newline(subject *subj)
{
	int nlpos = subj->pos;
	// skip over newline
	advance(subj);
	// skip spaces at beginning of line
	while (peek_char(subj) == ' ') {
		advance(subj);
	}
	if (nlpos > 1 &&
	    peek_at(subj, nlpos - 1) == ' ' &&
	    peek_at(subj, nlpos - 2) == ' ') {
		return make_linebreak();
	} else {
		return make_softbreak();
	}
}

static int subject_find_special_char(subject *subj, int options)
{
    //Added {} to the list of special characters
	// "\n\\`&_*[]<!{}"
	static const int8_t SPECIAL_CHARS[256] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1,
		1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, //
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};

	// " ' . -
	static const char SMART_PUNCT_CHARS[] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0,
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
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	};

	int n = subj->pos + 1;

	while (n < subj->input.len) {
		if (SPECIAL_CHARS[subj->input.data[n]])
            return n;
		if (options & CMARK_OPT_SMART &&
		    SMART_PUNCT_CHARS[subj->input.data[n]])
			return n;
		n++;
	}

	return subj->input.len;
}


// Parse an inline, advancing subject, and add it as a child of parent.
// Return 0 if no inline can be parsed, 1 otherwise.
static int 
parse_inline(subject* subj, cmark_node * parent, int options)
{
    cmark_node* new_inl = NULL;
    cmark_chunk contents;
    unsigned char c;
    int endpos;
    c = peek_char(subj);
    if (c == 0) {
	return 0;
    }
    switch(c) {
    case '\n':
	new_inl = handle_newline(subj);
	break;
    case '`':
	new_inl = handle_backticks(subj);
	break;
    case '\\':
	new_inl = handle_backslash(subj);
	break;
    case '&':
	new_inl = handle_entity(subj);
	break;
    case '<':
	new_inl = handle_pointy_brace(subj);
	break;
    case '*':
    case '_':
    case '\'':
    case '"':
	new_inl = handle_delim(subj, c, options & CMARK_OPT_SMART);
	break;
    case '-':
	new_inl = handle_hyphen(subj, options & CMARK_OPT_SMART);
	break;
    case '.':
	new_inl = handle_period(subj, options & CMARK_OPT_SMART);
	break;
    case '[':
	advance(subj);
	new_inl = make_str(cmark_chunk_literal("["));
	push_delimiter(subj, '[', true, false, new_inl);
	break;
    case ']':
	new_inl = handle_close_bracket(subj, parent);
	break;

    case '{':
        //go past the {
        advance(subj);
	new_inl = make_str(cmark_chunk_literal("{"));
	push_delimiter(subj, '{', true, false, new_inl);
	break;

    case '}':
        new_inl = handle_close_curly_brace(subj,parent);
        break;

    case '!':
	advance(subj);
	if (peek_char(subj) == '[') {
	    advance(subj);
	    new_inl = make_str(cmark_chunk_literal("!["));
	    push_delimiter(subj, '!', false, true, new_inl);
	} else {
	    new_inl = make_str(cmark_chunk_literal("!"));
	}
	break;
    default:
	endpos = subject_find_special_char(subj, options);
	contents = cmark_chunk_dup(&subj->input, subj->pos, endpos - subj->pos);
	if (cmark_verbose) cmark_chunk_show(&contents);
	subj->pos = endpos;

	// if we're at a newline, strip trailing spaces.
	if (peek_char(subj) == '\n') {
	    cmark_chunk_rtrim(&contents);
	}
	new_inl = make_str(contents);
    }
    if (new_inl != NULL) {
        cmark_node_append_child(parent, new_inl);
    }
    return 1;
}

// Parse inlines from parent's string_content, adding as children of parent.
extern void 
cmark_parse_inlines(cmark_node* parent, cmark_reference_map *refmap, int options)
{
    subject subj;
    subject_from_buf(&subj, &parent->string_content, refmap);
    //parse inline parses special character groups at a tme so [,],{,}
    //and other special characters are parsed as only single
    //characters when parse_inline is called
    while (!is_eof(&subj) && parse_inline(&subj, parent, options)) ;

    process_emphasis(&subj, NULL);
}

// Parse zero or more space characters, including at most one newline.
static void spnl(subject* subj)
{
	bool seen_newline = false;
	while (peek_char(subj) == ' ' ||
	       (!seen_newline &&
	        (seen_newline = peek_char(subj) == '\n'))) {
		advance(subj);
	}
}

// Parse reference.  Assumes string begins with '[' character.
// Modify refmap if a reference is encountered.
// Return 0 if no reference found, otherwise position of subject
// after reference is parsed.
int cmark_parse_reference_inline(cmark_strbuf *input, cmark_reference_map *refmap)
{
    subject subj;

    cmark_chunk lab;
    cmark_chunk url;
    cmark_chunk title;

    int matchlen = 0;
    int beforetitle;

    subject_from_buf(&subj, input, NULL);

    // parse label:
    //will parse only the stuff between [Link Label]
    if (!link_label(&subj, &lab))
	{
	    return 0;
	}
    // colon:
    if (peek_char(&subj) == ':') {
	advance(&subj);
    } else {
	return 0;
    }

    // parse link url:
    spnl(&subj);
    matchlen = scan_link_url(&subj.input, subj.pos);
    if (matchlen) {
	url = cmark_chunk_dup(&subj.input, subj.pos, matchlen);
	subj.pos += matchlen;
    } else {
	return 0;
    }

    // parse optional link_title
    beforetitle = subj.pos;
    spnl(&subj);
    matchlen = scan_link_title(&subj.input, subj.pos);
    if (matchlen) {
	title = cmark_chunk_dup(&subj.input, subj.pos, matchlen);
	subj.pos += matchlen;
    } else {
	subj.pos = beforetitle;
	title = cmark_chunk_literal("");
    }
    // parse final spaces and newline:
    while (peek_char(&subj) == ' ') {
	advance(&subj);
    }
    if (peek_char(&subj) == '\n') {
	advance(&subj);
    } else if (peek_char(&subj) != 0) {
	return 0;
    }
    // insert reference into refmap
    cmark_reference_create(refmap, &lab, &url, &title);
    return subj.pos;
}

// Parse includes.  Assumes string begins with '<<' character.
// Add an include node to the head of the document in case include is found
// Return 0 if no reference found, otherwise position of subject
// after include is parsed.
int cmark_parse_include_inline(cmark_strbuf *input,cmark_parser *parser)
{
    subject subj;
    cmark_chunk file;
    char *filename;
    
    int matchlen = 0;
    subject_from_buf(&subj,input,NULL);
    spnl(&subj);
    matchlen = scan_link_url(&subj.input, subj.pos);
    if (matchlen) {
	file = cmark_chunk_dup(&subj.input, subj.pos, matchlen);
	subj.pos += matchlen;
	//already will match 2 chars. So if we match less than 4 characters no way we can have <<...>>
	if(file.len<4)
	    {
		return 0;
	    }
	if(file.data[file.len-1]=='>' && file.data[file.len-2]=='>')
	    {
		filename = malloc(sizeof(char)*file.len);
		//            //advancing by 2 because don't want to include the << at the start
		memcpy(filename,(char*)file.data+2,file.len-2);
		filename[file.len-4] = '\0';
		cmark_add_to_head(parser->root,filename);
	    }
	else
	    {
		return 0;
	    }
    } else {
	return 0;
    }
    // parse final spaces and newline:
    while (peek_char(&subj) == ' ') {
	advance(&subj);
    }
    if (peek_char(&subj) == '\n') {
	advance(&subj);
    } else if (peek_char(&subj) != 0) {
	return 0;
    }
    free(filename);
    return subj.pos;
}

// Local Variables:
// mode: c           
// c-basic-offset: 4
// End:
