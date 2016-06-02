#ifndef CMARK_INLINES_H
#define CMARK_INLINES_H

#ifdef __cplusplus
extern "C" {
#endif

cmark_chunk cmark_clean_url(cmark_chunk *url);
cmark_chunk cmark_clean_title(cmark_chunk *title);

void cmark_parse_inlines(cmark_node* parent, cmark_reference_map *refmap, int options);

int cmark_parse_reference_inline(cmark_strbuf *input, cmark_reference_map *refmap);
    
int cmark_parse_include_inline(cmark_strbuf *input,cmark_parser *parser);
    
int cmark_parse_toc_inline(cmark_strbuf *input, cmark_parser *parser);

int cmark_parse_curly_1arg(cmark_strbuf *input,cmark_parser *parser, char* tag, cmark_node_type type);


#ifdef __cplusplus
}
#endif

#endif
