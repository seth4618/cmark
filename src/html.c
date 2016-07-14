#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "config.h"
#include "cmark.h"
#include "node.h"
#include "buffer.h"
#include "houdini.h"
#include "quest.h"

// Functions to convert cmark_nodes to HTML strings.
extern int cmark_verbose;

extern void outExamBlock(cmark_strbuf *html, cmark_node* node, int entering);


static void escape_html_but_blank(cmark_strbuf *dest, const unsigned char *source, int length)
{
	if (length < 0)
		length = strlen((char *)source);

	houdini_escape_html0_but_blank(dest, source, (size_t)length, 0);
}

static void escape_html(cmark_strbuf *dest, const unsigned char *source, int length)
{
	if (length < 0)
		length = strlen((char *)source);

	houdini_escape_html0(dest, source, (size_t)length, 0);
}

static void escape_href(cmark_strbuf *dest, const unsigned char *source, int length)
{
	if (length < 0)
		length = strlen((char *)source);

	houdini_escape_href(dest, source, (size_t)length);
}


static inline void cr(cmark_strbuf *html)
{
	if (html->size && html->ptr[html->size - 1] != '\n')
		cmark_strbuf_putc(html, '\n');
}

struct render_state {
	cmark_strbuf* html;
	cmark_node *plain;
    //Added the following fields to the render state to be able to render table of contents
    bool inside_toc;
    int prev_level;
    int open;
    bool start;
};

static void
S_render_sourcepos(cmark_node *node, cmark_strbuf *html, int options)
{
    if (CMARK_OPT_SOURCEPOS & options) {
        printf("Options matched\n");
	cmark_strbuf_printf(html, " data-sourcepos=\"%d:%d-%d:%d\"",
			    cmark_node_get_start_line(node),
			    cmark_node_get_start_column(node),
			    cmark_node_get_end_line(node),
			    cmark_node_get_end_column(node));
    }

    // see if there are any attributes
    if (node->attr != NULL) {
	cmark_strbuf_printf(html, " %s", cmark_chunk_to_cstr(&node->attr->as.literal));
	node->attr = NULL;
    }
}

//utility function to render a list normally, not of the type toc
static void render_list_normal(cmark_strbuf *html,cmark_node *node,int options)
{
    cmark_strbuf_puts(html, "<li");
    S_render_sourcepos(node, html, options);
    cmark_strbuf_putc(html, '>');
}


static int
S_render_node(cmark_node *node, cmark_event_type ev_type,
              struct render_state *state, int options)
{
    cmark_node *parent;
    cmark_node *grandparent;
    cmark_strbuf *html = state->html;
    char start_header[] = "<h0";
    char end_header[] = "</h0";
    bool tight;

    bool entering = (ev_type == CMARK_EVENT_ENTER);

    if (cmark_verbose) {
	fprintf(stderr, "%s:%s:%d", entering?"Enter>":"Exit< ", cmark_node_get_type_string(node), options);
	if (node->type == CMARK_NODE_TEXT) {
	    char* p = malloc(node->as.literal.len+1);
	    strncpy(p, (const char*)node->as.literal.data, node->as.literal.len);
	    p[node->as.literal.len] = 0;
	    fprintf(stderr, " [%s]", p);
	    free(p);
	}
	fprintf(stderr, "\n");
    }

    if (state->plain == node) {// back at original node
	state->plain = NULL;
    }

    if (state->plain != NULL) {
	switch(node->type) {
	case CMARK_NODE_TEXT:
	case CMARK_NODE_CODE:
	case CMARK_NODE_INLINE_HTML:
	    escape_html(html, node->as.literal.data,
			node->as.literal.len);
	    break;

	case CMARK_NODE_LINEBREAK:
	case CMARK_NODE_SOFTBREAK:
	    cmark_strbuf_putc(html, ' ');
	    break;
	default:
	    break;
	}
	return 1;
    }

    switch (node->type) {
    case CMARK_NODE_NONE:
	// skip this node
	break;

    case CMARK_NODE_DOCUMENT:
	break;

	//cr just adds a new line at the end of html if it doesn't exist
    case CMARK_NODE_BLOCK_QUOTE:
	if (entering) {
	    cr(html);
	    cmark_strbuf_puts(html, "<blockquote");
	    S_render_sourcepos(node, html, options);
	    cmark_strbuf_puts(html, ">\n");
	} else {
	    cr(html);
	    cmark_strbuf_puts(html, "</blockquote>\n");
	}
	break;

    case CMARK_NODE_LIST: {
	cmark_list_type list_type = node->as.list.list_type;
	int start = node->as.list.start;

	if (entering) {
	    cr(html);
	    if (list_type == CMARK_BULLET_LIST) {
		cmark_strbuf_puts(html, "<ul");
		S_render_sourcepos(node, html, options);
		cmark_strbuf_puts(html, ">\n");
	    } else if (start == 1) {
		cmark_strbuf_puts(html, "<ol");
		S_render_sourcepos(node, html, options);
		cmark_strbuf_puts(html, ">\n");
	    } else {
		cmark_strbuf_printf(html,
				    "<ol start=\"%d\"",
				    start);
		S_render_sourcepos(node, html, options);
		cmark_strbuf_puts(html, ">\n");
	    }
	} else {
	    cmark_strbuf_puts(html,
			      list_type == CMARK_BULLET_LIST ?
			      "</ul>\n" : "</ol>\n");
	}
	break;
    }
    
    case CMARK_NODE_QUESTION:
	if(entering) {
	    //cmark_strbuf_puts(html,"<question entering>");
	    char* num = updateAndFormatQuestion(node->as.qlevel, node);
	    cmark_strbuf_printf(html,"<span class=\"qnum%d\">%s</span>\n",
				node->as.qlevel, num);
	} else {
	    //cmark_strbuf_puts(html,"<question leaving>");
	}
	break;
	    
    case CMARK_NODE_BLANK:
	if (entering) {
	    format_blank_input(html, node, -1, -1);
	}
	break;
	
    case CMARK_NODE_ANSWER:
	if (entering) {
	    cmark_node* qtype = get_enclosing_exam(node);
	    format_exam_widget(html, qtype, node);
	}
	break;

    case CMARK_NODE_TOC:
	//special case if rendering the list of type toc
	/* the toc list is rendered as follows:
	   Suppose the text contained
	   <h1>h11</h1>
	   <h2>h21</h2>
	   <h3>h31</h3>
	   <h2>h22</h2>
	   <h1>h12</h1>
             
	   It will be rendered in html as nested ordered lists (ignoring the links)
	   <ol class = "toc">
	   <li><p>h11</p>
	   <ol>
	   <li><p>h21</p>
	   <ol>
	   <li><p>h31</p></li>
	   </ol>
	   </li>
	   <li><p>h22</p></li>
	   </ol>
	   </li>
	   <li><p>h12</p></li>
	   </ol>
                     
	*/
	if(entering)
	    {
		cmark_strbuf_puts(html,"<ol class=\"toc\">\n");
		state->inside_toc = true;
	    }
	else
	    {
		cmark_strbuf_puts(html,"</li>\n");
		if(state->open>0)
		    {
			while(state->open)
			    {
				cmark_strbuf_puts(html,"</ol>\n</li>\n");
				state->open-=1;
			    }
		    }
		cmark_strbuf_puts(html,"</ol>\n");
		state->inside_toc = false;
	    }
	break;

    case CMARK_NODE_ITEM:
	if (entering) {
	    cr(html);
	    if(state->inside_toc)
		{
		    int level = atoi(cmark_node_get_user_data(node));
		    if(level < state->prev_level)
			{
			    cmark_strbuf_puts(html,"</li>\n");
			    int diff = state->prev_level - level;
			    while(diff)
				{
				    cmark_strbuf_puts(html,"</ol>\n</li>\n");
				    diff-=1;
				    state->open-=1;
				}
			    state->prev_level = level;
			    cmark_strbuf_puts(html,"<li>\n");
			}
		    else if(level > state->prev_level)
			{
			    int diff = level - state->prev_level;
			    while(diff)
				{
				    cmark_strbuf_puts(html,"<ol>\n<li>\n");
				    diff-=1;
				    state->open+=1;
				}
			    state->prev_level = level;
			}
		    else
			{
			    if(state->start)
				{
				    render_list_normal(html,node,options);
				    state->start = false;
				    break;
				}
			    else
				{
				    cmark_strbuf_puts(html,"</li>\n");
				    render_list_normal(html,node,options);
				}
			}
		}
	    else
		{
		    render_list_normal(html,node,options);
		}
	} else {
	    if(!state->inside_toc)
		{
		    cmark_strbuf_puts(html, "</li>\n");
		}
	}
	break;

    case CMARK_NODE_HEADER:
	if (entering) {
	    cr(html);
	    start_header[2] = '0' + node->as.header.level;
	    cmark_strbuf_puts(html, start_header);
	    S_render_sourcepos(node, html, options);
	    if(cmark_node_get_user_data(node)!=NULL)
		{
		    cmark_strbuf_puts(html," id=\"");
		    cmark_strbuf_puts(html,cmark_node_get_user_data(node));
		    cmark_strbuf_putc(html,'"');
		}
	    cmark_strbuf_putc(html, '>');
	} else {
	    end_header[3] = '0' + node->as.header.level;
	    cmark_strbuf_puts(html, end_header);
	    cmark_strbuf_puts(html, ">\n");
	}
	break;

    case CMARK_NODE_CODE_BLOCK:
	cr(html);

	if (!node->as.code.fenced || node->as.code.info.len == 0) {
	    cmark_strbuf_puts(html, "<pre");
	    S_render_sourcepos(node, html, options);
	    cmark_strbuf_puts(html, "><code>");
	} else {
	    int first_tag = 0;
	    while (first_tag < node->as.code.info.len &&
		   node->as.code.info.data[first_tag] != ' ') {
		first_tag += 1;
	    }

	    cmark_strbuf_puts(html, "<pre");
	    S_render_sourcepos(node, html, options);
	    cmark_strbuf_puts(html, "><code class=\"language-");
	    escape_html_but_blank(html, node->as.code.info.data, first_tag);
	    cmark_strbuf_puts(html, "\">");
	}

	if (options & CMARK_OPT_EXAM) {
	    // do a {blank} hack
	    escape_html_but_blank(html, node->as.code.literal.data,
			node->as.code.literal.len);
	} else {
	    escape_html(html, node->as.code.literal.data,
			node->as.code.literal.len);
	}
	cmark_strbuf_puts(html, "</code></pre>\n");
	break;

    case CMARK_NODE_HTML:
	cr(html);
	cmark_strbuf_put(html, node->as.literal.data, node->as.literal.len);
	break;

    case CMARK_NODE_HRULE:
	cr(html);
	cmark_strbuf_puts(html, "<hr");
	S_render_sourcepos(node, html, options);
	cmark_strbuf_puts(html, " />\n");
	break;

    case CMARK_NODE_PARAGRAPH:
	parent = cmark_node_parent(node);
	grandparent = cmark_node_parent(parent);
	if (grandparent != NULL &&
	    grandparent->type == CMARK_NODE_LIST) {
	    tight = grandparent->as.list.tight;
	} else {
	    tight = false;
	}
	if (!tight) {
	    if (entering) {
		cr(html);
		cmark_strbuf_puts(html, "<p");
		S_render_sourcepos(node, html, options);
		cmark_strbuf_putc(html, '>');
	    } else {
		cmark_strbuf_puts(html, "</p>\n");
	    }
	}
	break;

    case CMARK_NODE_TEXT:
	escape_html(html, node->as.literal.data, node->as.literal.len);
	break;

    case CMARK_NODE_LINEBREAK:
	cmark_strbuf_puts(html, "<br />\n");
	break;

    case CMARK_NODE_SOFTBREAK:
	if (options & CMARK_OPT_HARDBREAKS) {
	    cmark_strbuf_puts(html, "<br />\n");
	} else {
	    cmark_strbuf_putc(html, '\n');
	}
	break;

    case CMARK_NODE_CODE:
	cmark_strbuf_puts(html, "<code>");
	escape_html(html, node->as.literal.data, node->as.literal.len);
	cmark_strbuf_puts(html, "</code>");
	break;

    case CMARK_NODE_INLINE_HTML:
	cmark_strbuf_put(html, node->as.literal.data, node->as.literal.len);
	break;

    case CMARK_NODE_STRONG:
	if (entering) {
	    cmark_strbuf_puts(html, "<strong>");
	} else {
	    cmark_strbuf_puts(html, "</strong>");
	}
	break;

    case CMARK_NODE_EMPH:
	if (entering) {
	    cmark_strbuf_puts(html, "<em>");
	} else {
	    cmark_strbuf_puts(html, "</em>");
	}
	break;
            
    case CMARK_NODE_INLINE_LINK:
	if(entering)
	    {
		//the data for the inline link is stored as
		//a literal in the node
		cmark_strbuf_puts(html,"<a ");
		S_render_sourcepos(node, html, options);
		cmark_strbuf_puts(html, " name = \"");
		cmark_strbuf_puts(html,cmark_node_get_literal(node));
		cmark_strbuf_puts(html,"\">");
	    }
	else{
	    cmark_strbuf_puts(html,"</a>");
	}
	break;


    case CMARK_NODE_LINK:
	if (entering) {
	    //backslash escaping string
	    cmark_strbuf_puts(html, "<a ");
	    S_render_sourcepos(node, html, options);
	    cmark_strbuf_puts(html, " href=\"");
	    escape_href(html, node->as.link.url.data,
			node->as.link.url.len);

	    if (node->as.link.title.len) {
		cmark_strbuf_puts(html, "\" title=\"");
		escape_html(html, node->as.link.title.data,
			    node->as.link.title.len);
	    }

	    cmark_strbuf_puts(html, "\">");
	} else {
	    cmark_strbuf_puts(html, "</a>");
	}
	break;
            
    case CMARK_NODE_IMAGE:
	if (entering) {
	    cmark_strbuf_puts(html, "<img ");
	    S_render_sourcepos(node, html, options);
	    cmark_strbuf_puts(html, " src=\"");
	    escape_href(html, node->as.link.url.data,
			node->as.link.url.len);

	    cmark_strbuf_puts(html, "\" alt=\"");
	    state->plain = node;
	} else {
	    if (node->as.link.title.len) {
		cmark_strbuf_puts(html, "\" title=\"");
		escape_html(html, node->as.link.title.data,
			    node->as.link.title.len);
	    }

	    cmark_strbuf_puts(html, "\" />");
	}
	break;
    case CMARK_NODE_HEAD:
	if(entering)
	    {
		cmark_strbuf_puts(html,"<head>\n");
	    }
	else
	    {
		cmark_strbuf_puts(html,"</head>\n");
	    }
	break;
    case CMARK_NODE_INCLUDE:
	if(entering)
	    {
		if(strstr(cmark_node_get_literal(node),".css"))
		    {
			cmark_strbuf_puts(html,"<link rel=\"stylesheet\" type = \"text/css\" href=\"");
			cmark_strbuf_puts(html,cmark_node_get_literal(node));
			cmark_strbuf_putc(html,'"');
		    }
		else
		    {
			cmark_strbuf_puts(html,"<script src = \"");
			cmark_strbuf_puts(html,cmark_node_get_literal(node));
			cmark_strbuf_puts(html,"\">");
		    }
            
	    }
	else
	    {
		if(strstr(cmark_node_get_literal(node),".css"))
		    cmark_strbuf_puts(html,">\n");
		else
		    cmark_strbuf_puts(html,"</script>\n");
	    }
	break;
    case CMARK_NODE_BODY:
	if(entering) {
		cmark_strbuf_puts(html,"<body>\n");
	    } else {
		cmark_strbuf_puts(html,"</body>\n");
	    }
	break;

    case CMARK_NODE_EXAM:
	outExamBlock(html, node, entering);
	break;

    case CMARK_NODE_CLEAR_SOLUTION:
	clearSolutionBlock(html, node, entering);
	break;

    case CMARK_NODE_SOLUTION:
	markSolutionBlock(html, node, entering);
	break;

    case CMARK_NODE_TABLE:
	if (entering) {
	    cmark_strbuf_puts(html,"\n<table");
	    S_render_sourcepos(node, html, options);
	    cmark_strbuf_putc(html, '>');
	} else
	    cmark_strbuf_puts(html,"</table>\n");
	break;

    case CMARK_NODE_ROW:
	if (entering) 
	    cmark_strbuf_puts(html,"\n<tr>");
	else
	    cmark_strbuf_puts(html,"</tr>\n");
	break;

    case CMARK_NODE_CELL:
	if (entering) 
	    cmark_strbuf_puts(html,"<td>");
	else
	    cmark_strbuf_puts(html,"</td>");
	break;

    default:
	fprintf(stderr, "Unknown node: %d (%s)\n", node->type, cmark_node_get_type_string(node));
	assert(false);
	break;
    }

    // cmark_strbuf_putc(html, 'x');
    return 1;
}

char *cmark_render_html(cmark_node *root, int options)
{
	char *result;
	cmark_strbuf html = GH_BUF_INIT;
	cmark_event_type ev_type;
	cmark_node *cur;
	
	if (options & CMARK_OPT_EXAM) {
	    create_exam_output_buffer(root);
	}

	struct render_state state = { &html, NULL, false,1,0,true};
	cmark_iter *iter = cmark_iter_new(root);

	while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
		cur = cmark_iter_get_node(iter);
		S_render_node(cur, ev_type, &state, options);
	}
	result = (char *)cmark_strbuf_detach(&html);

	cmark_iter_free(iter);

	// check that all attributes got processed
	iter = cmark_iter_new(root);
	int error = 0;
	while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
		cur = cmark_iter_get_node(iter);
		if (cur->attr != NULL) {
		    fprintf(stderr, "%s has attribute [%s] which was not rendered - call seth\n", 
			    cmark_node_get_type_string(cur),
			    cmark_chunk_to_cstr(&cur->attr->as.literal));
		    error++;
		}
	}
	if (error) exit(-1);
	cmark_iter_free(iter);

	return result;
}

// Local Variables:
// mode: c           
// c-basic-offset: 4
// End:
