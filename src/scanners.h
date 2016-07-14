#include "cmark.h"
#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

//int scan_curlytag_inline(cmark_chunk* p, int pos, const char* tag);

int _scan_at(int (*scanner)(const unsigned char *), cmark_chunk *c, int offset);
int _scan_scheme(const unsigned char *p);
int _scan_autolink_uri(const unsigned char *p);
int _scan_autolink_email(const unsigned char *p);
int _scan_autolink_inline(const unsigned char *p);
int _scan_html_tag(const unsigned char *p);
int _scan_html_block_tag(const unsigned char *p);
int _scan_link_url(const unsigned char *p);
int _scan_link_title(const unsigned char *p);
int _scan_spacechars(const unsigned char *p);
int _scan_atx_header_start(const unsigned char *p);
int _scan_setext_header_line(const unsigned char *p);
int _scan_hrule(const unsigned char *p);
int _scan_open_code_fence(const unsigned char *p);
int _scan_close_code_fence(const unsigned char *p);
int _scan_entity(const unsigned char *p);
int _scan_toc_inline(const unsigned char *p);
int _scan_question_qwidget(const unsigned char *p);
int _scan_solution(const unsigned char *p);
int _scan_solutionClear(const unsigned char *p);
int _scan_table(const unsigned char *p);
int _scan_attribute(const unsigned char *p);

#define scan_scheme(c, n) _scan_at(&_scan_scheme, c, n)
#define scan_autolink_uri(c, n) _scan_at(&_scan_autolink_uri, c, n)
#define scan_autolink_email(c, n) _scan_at(&_scan_autolink_email, c, n)
#define scan_autolink_inline(c,n) _scan_at(&_scan_autolink_inline, c, n)
#define scan_html_tag(c, n) _scan_at(&_scan_html_tag, c, n)
#define scan_html_block_tag(c, n) _scan_at(&_scan_html_block_tag, c, n)
#define scan_link_url(c, n) _scan_at(&_scan_link_url, c, n)
#define scan_link_title(c, n) _scan_at(&_scan_link_title, c, n)
#define scan_spacechars(c, n) _scan_at(&_scan_spacechars, c, n)
#define scan_atx_header_start(c, n) _scan_at(&_scan_atx_header_start, c, n)
#define scan_setext_header_line(c, n) _scan_at(&_scan_setext_header_line, c, n)
#define scan_hrule(c, n) _scan_at(&_scan_hrule, c, n)
#define scan_open_code_fence(c, n) _scan_at(&_scan_open_code_fence, c, n)
#define scan_close_code_fence(c, n) _scan_at(&_scan_close_code_fence, c, n)
#define scan_entity(c, n) _scan_at(&_scan_entity, c, n)
#define scan_toc(c, n) _scan_at(&_scan_toc_inline, c, n)
#define scan_question_qwidget(c, n) _scan_at(&_scan_question_qwidget, c, n)
#define scan_solution(c, n) _scan_at(&_scan_solution, c, n)
#define scan_solutionClear(c, n) _scan_at(&_scan_solutionClear, c, n)
#define scan_table(c, n) _scan_at(&_scan_table, c, n)
#define scan_attribute(c, n) _scan_at(&_scan_attribute, c, n)

#ifdef __cplusplus
}
#endif

// Local Variables:
// mode: c           
// c-basic-offset: 4
// End:

