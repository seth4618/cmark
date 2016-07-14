#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "cmark_ctype.h"
#include "config.h"
#include "parser.h"
#include "cmark.h"
#include "node.h"
#include "references.h"
#include "utf8.h"
#include "scanners.h"
#include "inlines.h"
#include "houdini.h"
#include "buffer.h"
#include "debug.h"
#include <stdarg.h>

int cmark_verbose;

#define CODE_INDENT 4
#define peek_at(i, n) (i)->data[n]

#define DEBUG

#ifdef DEBUG
#define dbg_printf(...) printf(__VA_ARGS__)
#else
#define dbg_printf(...)
#endif

static void
die(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(-1);
}

static void
S_parser_feed(cmark_parser *parser, const unsigned char *buffer, size_t len,
              bool eof);

void print_nodes(cmark_node *root);

static void
S_process_line(cmark_parser *parser, const unsigned char *buffer,
               size_t bytes);

static cmark_node* make_block(cmark_node_type tag, int start_line, int start_column)
{
    cmark_node* e;
    
    e = newNode(tag);

    e->open = true;
    e->start_line = start_line;
    e->start_column = start_column;
    e->end_line = start_line;
    cmark_strbuf_init(&e->string_content, 32);
    
    return e;
}

void
print_tree(cmark_node* node, char* prompt)
{
    cmark_iter *iter = cmark_iter_new(node);
    cmark_node *cur;
    cmark_event_type ev_type;

    fprintf(stderr, "\n==========> %s\n", prompt);
    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
	cur = cmark_iter_get_node(iter);
	bool entering = (ev_type == CMARK_EVENT_ENTER);
		
	fprintf(stderr, "%s:%d.%d:%s", entering?"Enter>":"Exit< ", 
		cur->start_line, cur->start_column, cmark_node_get_type_string(cur));
	if (entering) {
	    if (cur->type == CMARK_NODE_TEXT) {
		char* p = malloc(cur->as.literal.len+1);
		strncpy(p, (const char*)cur->as.literal.data, cur->as.literal.len);
		p[cur->as.literal.len] = 0;
		fprintf(stderr, " [%s]", p);
		free(p);
	    } else if (cur->type == CMARK_NODE_PARAGRAPH) {
		fprintf(stderr, " [%s]", cur->string_content.ptr);
	    } else if (cur->type == NODE_CODE_BLOCK) {
		fprintf(stderr, " [%s]", cmark_chunk_to_cstr(&cur->as.code.literal));
	    } else if (cur->type == NODE_SOLUTION) {
		fprintf(stderr, " %d[%s]", cur->as.solution.stanza, cmark_chunk_to_cstr(&cur->as.solution.literal));
	    } 
	}
	fprintf(stderr, "\n");
    }
    fprintf(stderr, "==========< %s\n", prompt);
}


/* add_toc_item
 params:
 toc: te node that is of type toc
 label: The text of the entry in the table of contents
 link: the link to the header as a string
 level: the level of the header */

/* This function adds an item to the toc. The toc is essentially just like a ordered list, except it is of class toc so that people can customize it. This function adds children to the ordered list, by first creating a NODE_ITEM and populating it with the text of the item as a paragraph and the link of the item as a NODE_LINK. 
 
    The user_data field of the node_item contains a string that specifies the level.
    The paragraph node also has a child node which contains the url. The url is simply a string called #link and can be rendered normally as <a href = >
 */
void add_toc_item(cmark_node *toc,const char *label,char *link,int level)
{
    cmark_node *new_item = cmark_node_new(NODE_ITEM);
    cmark_node *par = cmark_node_new(NODE_PARAGRAPH);
    cmark_node *url = cmark_node_new(NODE_LINK);
    cmark_node *name = cmark_node_new(NODE_TEXT);
    cmark_chunk *chunk = calloc(1,sizeof(cmark_chunk));
    chunk->data = malloc(sizeof(char)*100);
    sprintf((char*)chunk->data,"#%s",link);
    //additional 1 for the extra #character
    chunk->len = strlen(link)+1;
    url->as.link.url = *chunk;
    new_item->user_data = malloc(sizeof(char)*40);
    sprintf(new_item->user_data,"%d",level);
    cmark_node_set_literal(name,label);
    cmark_node_append_child(url,name);
    cmark_node_append_child(par,url);
    cmark_node_append_child(new_item,par);
    cmark_node_append_child(toc,new_item);
}

/*add_toc
/params: 
 toc: the node in the AST that is of type NODE_TOC
 root: the starting root node of the AST
 maxDepth: the depth if specified after the toc eg: {toc:4}
 */

/*This functions walks through the whole tree, and every time it encounters a header of valid level, it calls add_toc item on the node */
void add_toc(cmark_node *toc, cmark_node *root,int maxDepth)
{
    cmark_iter *iter = cmark_iter_new(root);
    cmark_event_type ev_type;
    int level;
    while((ev_type = cmark_iter_next(iter))!=CMARK_EVENT_DONE)
    {
        if(ev_type==CMARK_EVENT_ENTER)
        {
            cmark_node *node = cmark_iter_get_node(iter);
            if(node->type==NODE_HEADER && node->as.header.level <= maxDepth)
            {
                level = node->as.header.level;
                char *url = (char *)cmark_node_get_user_data(node);
                cmark_node *child = node->first_child;
                while(child)
                {
                    if(child->type==NODE_TEXT)
                    {
                        //the url parameter that is passed is the user_data of the header that contains string like "tocX"
                        add_toc_item(toc,cmark_node_get_literal(child),url,level);
                        break;
                    }
                    child=child->next;
                }
            }
        }
    }
    cmark_iter_free(iter);
}

// Create a root document node.
static cmark_node* make_document()
{
    cmark_node *e = make_block(NODE_DOCUMENT, 1, 1);
    return e;
}

cmark_parser *cmark_parser_new(int options)
{
    cmark_parser *parser = (cmark_parser*)malloc(sizeof(cmark_parser));
    cmark_node *document = make_document();
    cmark_strbuf *line = (cmark_strbuf*)malloc(sizeof(cmark_strbuf));
    cmark_strbuf *buf  = (cmark_strbuf*)malloc(sizeof(cmark_strbuf));
    cmark_strbuf_init(line, 256);
    cmark_strbuf_init(buf, 0);

    installQtypes();

    parser->refmap = cmark_reference_map_new();
    parser->root = document;
    parser->current = document;
    parser->line_number = 0;
    parser->curline = line;
    parser->last_line_length = 0;
    parser->linebuf = buf;
    parser->options = options;
    
    return parser;
}

void cmark_parser_free(cmark_parser *parser)
{
    cmark_strbuf_free(parser->curline);
    free(parser->curline);
    cmark_strbuf_free(parser->linebuf);
    free(parser->linebuf);
    cmark_reference_map_free(parser->refmap);
    free(parser);
}

static cmark_node*
finalize(cmark_parser *parser, cmark_node* b);

// Returns true if line has only space characters, else false.
static bool is_blank(cmark_strbuf *s, int offset)
{
    while (offset < s->size) {
        switch (s->ptr[offset]) {
            case '\n':
                return true;
            case ' ':
                offset++;
                break;
            default:
                return false;
        }
    }
    
    return true;
}

static inline bool can_contain(cmark_node_type parent_type, cmark_node_type child_type)
{
    return ( parent_type == NODE_DOCUMENT ||
            parent_type == NODE_BLOCK_QUOTE ||
            parent_type == NODE_ITEM ||
	     parent_type == NODE_TABLE ||
	     parent_type == NODE_SOLUTION ||
            (parent_type == NODE_LIST && child_type == NODE_ITEM) ||
	     ((parent_type == NODE_EXAM) && (child_type != NODE_EXAM)) );
}

static inline bool accepts_lines(cmark_node_type block_type)
{
    return (block_type == NODE_PARAGRAPH ||
	    block_type == NODE_SOLUTION ||
            block_type == NODE_HEADER ||
            block_type == NODE_CODE_BLOCK);
}

static void add_line(cmark_node* node, cmark_chunk *ch, int offset)
{
    assert(node->open);
    cmark_strbuf_put(&node->string_content, ch->data + offset, ch->len - offset);
}

static void remove_trailing_blank_lines(cmark_strbuf *ln)
{
    int i;
    
    for (i = ln->size - 1; i >= 0; --i) {
        unsigned char c = ln->ptr[i];
        
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
    }
    
    if (i < 0) {
        cmark_strbuf_clear(ln);
        return;
    }
    
    i = cmark_strbuf_strchr(ln, '\n', i);
    if (i >= 0)
        cmark_strbuf_truncate(ln, i);
}

// Check to see if a node ends with a blank line, descending
// if needed into lists and sublists.
static bool ends_with_blank_line(cmark_node* node)
{
    cmark_node *cur = node;
    while (cur != NULL) {
        if (cur->last_line_blank) {
            return true;
        }
        if (cur->type == NODE_LIST || cur->type == NODE_ITEM) {
            cur = cur->last_child;
        } else {
            cur = NULL;
        }
    }
    return false;
}

// Break out of all containing lists
static int break_out_of_lists(cmark_parser *parser, cmark_node ** bptr)
{
    cmark_node *container = *bptr;
    cmark_node *b = parser->root;
    // find first containing NODE_LIST:
    while (b && b->type != NODE_LIST) {
        b = b->last_child;
    }
    if (b) {
        while (container && container != b) {
            container = finalize(parser, container);
        }
        finalize(parser, b);
        *bptr = b->parent;
    }
    return 0;
}


static cmark_node*
finalize(cmark_parser *parser, cmark_node* b)
{
    int firstlinelen;
    int pos;
    cmark_node* item;
    cmark_node* subitem;
    cmark_node* parent;
    
    parent = b->parent;
    
    assert(b->open);  // shouldn't call finalize on closed blocks
    b->open = false;
    
    if (parser->curline->size == 0) {
        // end of input - line number has not been incremented
        b->end_line = parser->line_number;
        b->end_column = parser->last_line_length;
    } else if (b->type == NODE_DOCUMENT ||
	       b->type == NODE_SOLUTION ||
               (b->type == NODE_CODE_BLOCK && b->as.code.fenced) ||
               (b->type == NODE_HEADER && b->as.header.setext)) {
        b->end_line = parser->line_number;
        b->end_column = parser->curline->size -
	    (parser->curline->ptr[parser->curline->size - 1] == '\n' ?
	     1 : 0);
    } else {
        b->end_line = parser->line_number - 1;
        b->end_column = parser->last_line_length;
    }
    
    switch (b->type) {
    case NODE_PARAGRAPH:
	if(cmark_strbuf_at(&b->string_content,0)=='[')
            {
                while (cmark_strbuf_at(&b->string_content, 0) == '[' &&
                       (pos = cmark_parse_reference_inline(&b->string_content, parser->refmap))) {
                    cmark_strbuf_drop(&b->string_content, pos);
                }
                if (is_blank(&b->string_content, 0)) {
                    // remove blank node (former reference def)
                    cmark_node_free(b);
                }
            }
	else if(cmark_strbuf_at(&b->string_content,0)=='<' && cmark_strbuf_at(&b->string_content,1)=='<')
            {
                while(cmark_strbuf_at(&b->string_content,0)=='<' && cmark_strbuf_at(&b->string_content,1)=='<' && (pos = cmark_parse_include_inline(&b->string_content,parser)))
		    {
			cmark_strbuf_drop(&b->string_content,pos);
		    }
                if(is_blank(&b->string_content,0))
		    {
			cmark_node_free(b);
		    }
            }
	break;
            
    case NODE_SOLUTION:
	remove_trailing_blank_lines(&b->string_content);
	//cmark_strbuf_putc(&b->string_content, '\n');
	b->as.solution.literal = cmark_chunk_buf_detach(&b->string_content);
	break;

    case NODE_CODE_BLOCK:
	if (!b->as.code.fenced) { // indented code
	    remove_trailing_blank_lines(&b->string_content);
	    cmark_strbuf_putc(&b->string_content, '\n');
	} else {
                
	    // first line of contents becomes info
	    firstlinelen = cmark_strbuf_strchr(&b->string_content, '\n', 0);
                
	    cmark_strbuf tmp = GH_BUF_INIT;
	    houdini_unescape_html_f(
				    &tmp,
				    b->string_content.ptr,
				    firstlinelen
				    );
	    cmark_strbuf_trim(&tmp);
	    cmark_strbuf_unescape(&tmp);
	    b->as.code.info = cmark_chunk_buf_detach(&tmp);
                
	    cmark_strbuf_drop(&b->string_content, firstlinelen + 1);
	}
	b->as.code.literal = cmark_chunk_buf_detach(&b->string_content);
	break;
            
    case NODE_HTML:
	b->as.literal = cmark_chunk_buf_detach(&b->string_content);
	break;
            
    case NODE_LIST: // determine tight/loose status
	b->as.list.tight = true; // tight by default
	item = b->first_child;
            
	while (item) {
	    // check for non-final non-empty list item ending with blank line:
	    if (item->last_line_blank && item->next) {
		b->as.list.tight = false;
		break;
	    }
	    // recurse into children of list item, to see if there are
	    // spaces between them:
	    subitem = item->first_child;
	    while (subitem) {
		if (ends_with_blank_line(subitem) &&
		    (item->next || subitem->next)) {
		    b->as.list.tight = false;
		    break;
		}
		subitem = subitem->next;
	    }
	    if (!(b->as.list.tight)) {
		break;
	    }
	    item = item->next;
	}
            
	break;
            
    default:
	break;
    }
    return parent;
}

// Add a node as child of another.  Return pointer to child.
static void
add_already_created_child(cmark_parser *parser, cmark_node* parent,
			  cmark_node* child)
{
    assert(parent);
    
    // if 'parent' isn't the kind of node that can accept this child,
    // then back up til we hit a node that can.
    while (!can_contain(parent->type, child->type)) {
        parent = finalize(parser, parent);
    }
    
    child->parent = parent;
    
    if (parent->last_child) {
        parent->last_child->next = child;
        child->prev = parent->last_child;
    } else {
        parent->first_child = child;
        child->prev = NULL;
    }
    parent->last_child = child;
}

// Add a node as child of another.  Return pointer to child.
static cmark_node* add_child(cmark_parser *parser, cmark_node* parent,
                             cmark_node_type block_type, int start_column)
{
    cmark_node* child = make_block(block_type, parser->line_number, start_column);
    add_already_created_child(parser, parent, child);
    return child;
}


// Walk through node and all children, recursively, parsing
// string content into inline content where appropriate.
static void process_inlines(cmark_node* root, cmark_reference_map *refmap, int options)
{
    cmark_iter *iter = cmark_iter_new(root);
    cmark_node *cur;
    cmark_event_type ev_type;
    
    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cur = cmark_iter_get_node(iter);
	if (ev_type == CMARK_EVENT_ENTER) {
	    if (cur->type == NODE_PARAGRAPH ||
		cur->type == NODE_EXAM ||
		cur->type == NODE_HEADER) {
		cmark_parse_inlines(cur, refmap, options);
	    }
	}
    }
    
    cmark_iter_free(iter);
}

// Attempts to parse a list item marker (bullet or enumerated).
// On success, returns length of the marker, and populates
// data with the details.  On failure, returns 0.
static int parse_list_marker(cmark_chunk *input, int pos, cmark_list **dataptr)
{
    unsigned char c;
    int startpos;
    cmark_list *data;
    
    startpos = pos;
    c = peek_at(input, pos);
    
    if (c == '*' || c == '-' || c == '+') {
        pos++;
        if (!cmark_isspace(peek_at(input, pos))) {
            return 0;
        }
        data = (cmark_list *)calloc(1, sizeof(*data));
        if(data == NULL) {
            return 0;
        } else {
            data->marker_offset = 0; // will be adjusted later
            data->list_type = CMARK_BULLET_LIST;
            data->bullet_char = c;
            data->start = 1;
            data->delimiter = CMARK_PERIOD_DELIM;
            data->tight = false;
        }
    } else if (cmark_isdigit(c)) {
        int start = 0;
        
        do {
            start = (10 * start) + (peek_at(input, pos) - '0');
            pos++;
        } while (cmark_isdigit(peek_at(input, pos)));
        
        c = peek_at(input, pos);
        if (c == '.' || c == ')') {
            pos++;
            if (!cmark_isspace(peek_at(input, pos))) {
                return 0;
            }
            data = (cmark_list *)calloc(1, sizeof(*data));
            if(data == NULL) {
                return 0;
            } else {
                data->marker_offset = 0; // will be adjusted later
                data->list_type = CMARK_ORDERED_LIST;
                data->bullet_char = 0;
                data->start = start;
                data->delimiter = (c == '.' ? CMARK_PERIOD_DELIM : CMARK_PAREN_DELIM);
                data->tight = false;
            }
        } else {
            return 0;
        }
        
    } else {
        return 0;
    }
    *dataptr = data;
    return (pos - startpos);
}

// Return 1 if list item belongs in list, else 0.
static int lists_match(cmark_list *list_data, cmark_list *item_data)
{
    return (list_data->list_type == item_data->list_type &&
            list_data->delimiter == item_data->delimiter &&
            // list_data->marker_offset == item_data.marker_offset &&
            list_data->bullet_char == item_data->bullet_char);
}

//This function will encompass everything except the head node inside a document node and return a new node if a head node exists. Otherwise it will return the old node
cmark_node *add_body(cmark_node *root)
{
    if(root->type!=NODE_DOCUMENT)
    {
        fprintf(stderr,"Body can only be added to document node \n");
        exit(1);
    }
    assert(root->type==NODE_DOCUMENT);
    if(root->first_child->type==NODE_HEAD)
    {
        cmark_node *head = root->first_child;
        cmark_node_unlink(root->first_child);
        root->type = NODE_BODY;
        cmark_node *new_root = cmark_node_new(NODE_DOCUMENT);
        cmark_node_append_child(new_root,root);
        //reset the parameters of the old document
        new_root->start_line = root->start_line;
        new_root->start_column = root->start_column;
        new_root->end_line = root->end_line;
        new_root->end_column = root->end_column;
        new_root->open = root->open;
        new_root->last_line_blank = root->last_line_blank;
        cmark_node_prepend_child(new_root,head);
        return root->parent;
    }
    else
    {
        return root;
    }
    
}

/* add_header_links:
    params:
    root: starting root node of AST
    maxDepth: maximum allowed depth if specified
 */

/* this function will walk the tree and whenever it encounters a
 * header of appropriate level, it adds a string to the user data of
 * the form tocX where X is the number of the link. When being
 * rendered, check if the header contains any user_data and if it
 * does, render it as <h1 name = "tocX">. I chose to add the name
 * attribute to the header tag rather than the name because its easier
 * than adding another <a> tag as a child of the header tag*/
void add_header_links(cmark_node *root,int maxDepth)
{
    cmark_iter *iter = cmark_iter_new(root);
    cmark_event_type ev_type;
    int count = 0;
    while((ev_type = cmark_iter_next(iter))!=CMARK_EVENT_DONE)
          {
              if(ev_type==CMARK_EVENT_ENTER)
              {
                  cmark_node *node = cmark_iter_get_node(iter);
                  if(node->type==NODE_HEADER && node->as.header.level<=maxDepth)
                  {
                      char *user_data = malloc(sizeof(char)*40);
                      sprintf(user_data,"toc%d",count);
                      cmark_node_set_user_data(node,user_data);
                      count+=1;
                  }
              }
          }
    cmark_iter_free(iter);
}


/* toc_present:
    params:
    root: root node of AST */

/* This function will walk the tree of nodes using an iterator and
 * returns a node of the type TOC if it encounters it. Used to
 * determine if the document has a TOC node 
 */
cmark_node *toc_present(cmark_node *root)
{
    cmark_iter *iter = cmark_iter_new(root);
    cmark_event_type ev_type;
    while((ev_type = cmark_iter_next(iter))!=CMARK_EVENT_DONE)
    {
        if(ev_type==CMARK_EVENT_ENTER)
        {
            cmark_node *node = cmark_iter_get_node(iter);
            if(node->type==NODE_TOC)
            {
                cmark_iter_free(iter);
                return node;
            }
        }
    }
    cmark_iter_free(iter);
    return NULL;
}

static void
append_as_child(cmark_node* parent, cmark_node* child)
{
    child->parent = parent;
    child->prev = parent->last_child;
    child->next = NULL;

    if (parent->last_child) {
	parent->last_child->next = child;
	parent->last_child = child;
    } else {
	assert (parent->first_child == NULL);
	parent->first_child = parent->last_child = child;
    }
}

static cmark_node*
addTableElement(cmark_node_type tag, cmark_node* table)
{
    cmark_node* newrow = make_block(tag, 0, 0);
    append_as_child(table, newrow);
    return newrow;
}

static void
addCellLiteral(cmark_node* cell, char* text)
{
    cmark_node* n = make_block(NODE_TEXT, 0, 0);
    n->as.literal = cmark_chunk_literal(text);
    append_as_child(cell, n);
}

static cmark_node*
add2cell(cmark_node* cell, char* text, const char* sep)
{
    while (*text != 0) {
	char* p = strstr(text, sep);
	if (p) {
	    *p = 0;
	    if (text[0] == 0) {
		// empty cell
		addCellLiteral(cell, " ");
	    } else {
		addCellLiteral(cell, text);
	    }
	    cell = addTableElement(NODE_CELL, cell->parent);
	    p += strlen(sep);
	    text = p;
	} else {
	    if (strlen(text) > 0) addCellLiteral(cell, text);
	    return cell;
	}
    }
    return cell;
}

static int depth = 0;

// Walk through node and all children, recursively, parsing
// table nodes into tables.  convert text between softbreak or hardbreak into rows.
// since we rewrite the children of TABLE, we do this recursively
static void 
process_tables(cmark_node* node)
{
    if (cmark_verbose) {
	depth++;
	fprintf(stderr, "%*s %3d.%3d:%s\n", depth, "",  node->start_line, node->start_column, cmark_node_get_type_string(node));
    }
    if (node->type != NODE_TABLE) {
	if (node->next) process_tables(node->next);
	if (node->first_child) process_tables(node->first_child);
	if (cmark_verbose) depth--;
	return;
    }
    // we are at a NODE_TABLE
    if (cmark_verbose) fprintf(stderr, "Found One: %d.%d:%s -> %s\n", node->start_line, node->start_column, cmark_node_get_type_string(node), cmark_node_get_type_string(node->next));
    
    if (node->first_child == NULL) {
	process_tables(node->next);
	if (cmark_verbose) depth--;
	return;
    }

    if (cmark_verbose) fprintf(stderr, "Processing: %d.%d:%s -> %s\n", node->start_line, node->start_column, cmark_node_get_type_string(node), cmark_node_get_type_string(node->next));

    // there is actual content
    cmark_node* table = node;
    char* sepString = table->as.table.sep;
    cmark_node* rows = node->first_child;
    // disconnect all the children from the table node
    for (cmark_node* n = rows; n; n=n->next) n->parent = NULL;
    node->first_child = node->last_child = NULL;

    int maxchildren = 10;
    cmark_node** children = calloc(maxchildren, sizeof(cmark_node*));
    for (cmark_node* n = rows; n; n=n->next) {
	if (cmark_verbose) fprintf(stderr, "Beginning a new row node: [%s] @ %d\n", cmark_node_get_type_string(n), cmark_node_get_start_line(n));
	// each row node begins a new row, but may contain many rows inside it
	if (n->type != NODE_PARAGRAPH) {
	    if (n->type == NODE_CODE_BLOCK) {
		fprintf(stderr, "Error:%d:Encountered a code block at start of a table, too many spaces at start of line??\n", node->start_line);
		exit(-1);
	    } else {
		fprintf(stderr, "Internal Error:%d:Encountered a %s at start of a table. Did you close the table? If so, please report to seth\n", node->start_line, cmark_node_get_type_string(n));	    
		exit(-1);
	    }
	}
	assert (n->type == NODE_PARAGRAPH); /* for now this is all I know how to process */
	cmark_node* newrow = addTableElement(NODE_ROW, table);
	cmark_node* newcell = addTableElement(NODE_CELL, newrow);
	// allocate space for every child of this row node
	int cnt = 0;
	for (cmark_node* cur = n->first_child; cur; cur=cur->next) cnt++;
	if (cnt > maxchildren) {
	    maxchildren = cnt*2;
	    children = realloc(children, maxchildren*sizeof(cmark_node*));
	}
	// put a ref to each of the children of this row node in the children[]
	cnt = 0;
	for (cmark_node* cur = n->first_child; cur; cur=cur->next) {
	    children[cnt++] = cur;
	}
	// detach each of the children from their sibs and parent
	for (int i=0; i<cnt; i++) {
	    children[i]->next = children[i]->prev = children[i]->parent = NULL;
	}
	// empty out the row node itself
	n->first_child = n->last_child = NULL;
	// now process the children nodes one at a time
	for (int i=0; i<cnt; i++) {
	    cmark_node* cur = children[i];
	    if (cmark_verbose) fprintf(stderr, "[%s]\n", cmark_node_get_type_string(cur));
	    switch (cur->type) {
	    case NODE_TEXT:
		newcell = add2cell(newcell, (char*)cmark_node_get_literal(cur), sepString);
		break;

	    case NODE_SOFTBREAK:
	    case NODE_LINEBREAK:
		// check for last cell being empty
		if (newcell->first_child == NULL) {
		    if (newcell->prev == NULL) {
			// two cases.  If it is first line of table, ignore.  Otherwise just issue warning to user and continue
			if (newrow->prev != NULL) {
			    fprintf(stderr, "Empty row??\n"); exit(-1);
			} else {
			    // this is just a blank line at start of table
			    break;
			}
		    } 
		    newcell->prev->next = NULL;
		    newrow->last_child = newcell->prev;
		    newcell->parent = NULL;
		    newcell->prev = NULL;
		}
		newrow = addTableElement(NODE_ROW, table);
		newcell = addTableElement(NODE_CELL, newrow);
		break;

	    default:
		process_tables(cur);
		append_as_child(newcell, cur);
	    }
	}
	if (cmark_verbose) fprintf(stderr, "[--------]\n");
    }
    free(children);
    if (cmark_verbose) depth--;
    if (node->next) process_tables(node->next);
}

static int
attributes_follow(cmark_node* node)
{
    if (node == NULL) return 0;
    switch (node->type) {
    case CMARK_NODE_CODE:
    case CMARK_NODE_QUESTION:
    case CMARK_NODE_ANSWER:	
    case CMARK_NODE_BLANK:	
    case CMARK_NODE_EMPH:
    case CMARK_NODE_STRONG:
    case CMARK_NODE_LINK:
    case CMARK_NODE_INLINE_LINK:
    case CMARK_NODE_TABLE:
    case CMARK_NODE_IMAGE:
	return 1;
    default:
	return 0;
    }
    return 0;
}

static int
attributes_under(cmark_node* node)
{
    if (node == NULL) return 0;
    switch (node->type) {
    case CMARK_NODE_DOCUMENT:
    case CMARK_NODE_HEAD:
    case CMARK_NODE_BODY:
    case CMARK_NODE_BLOCK_QUOTE:
    case CMARK_NODE_LIST:
    case CMARK_NODE_ITEM:
    case CMARK_NODE_CODE_BLOCK:
    case CMARK_NODE_HTML:
    case CMARK_NODE_INCLUDE:
    case CMARK_NODE_TOC:
    case CMARK_NODE_EXAM:	/* new node which holds exam blocks */
    case CMARK_NODE_SOLUTION:	/* new node which holds solution blocks */
    case CMARK_NODE_ROW:
    case CMARK_NODE_CELL:
    case CMARK_NODE_PARAGRAPH:
    case CMARK_NODE_HEADER:
    case CMARK_NODE_HRULE:
	return 1;
    default:
	return 0;
    }
    return 0;
}

// remove any NONE nodes
static void 
clean_tree(cmark_node* node)
{
    cmark_iter *iter = cmark_iter_new(node);
    cmark_node *cur;
    cmark_event_type ev_type;
    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
	cur = cmark_iter_get_node(iter);
	bool entering = (ev_type == CMARK_EVENT_ENTER);
	if (!entering) continue;
	// first deal with children nodes
	while ((cur->first_child)&&(cur->first_child->type == CMARK_NODE_NONE)) {
	    cmark_node* none = cur->first_child;

	    if (cmark_verbose) fprintf(stderr, "Zapping child of [%s]\n",  cmark_node_get_type_string(cur));

	    cur->first_child = none->next;
	    if (none->next) {
		none->next->prev = NULL;
	    }
	    // should free up this sucker
	}
	// now deal with sibling nodes
	while ((cur->next)&&(cur->next->type == CMARK_NODE_NONE)) {
	    cmark_node* none = cur->next;

	    fprintf(stderr, "Zapping Sibling of [%s]\n",  cmark_node_get_type_string(cur));

	    cur->next = none->next;
	    if (none->next) {
		none->next->prev = cur;
	    } else {
		// check that parent has proper last subling pointer
		if (none->parent->last_child == none) {
		    fprintf(stderr, "fixing parent lastchild\n");
		    none->parent->last_child = cur;
		}
	    }
	    // should free up this sucker
	}
    }
}

// Walk through node and all children, recursively, parsing attributes and attaching them to the appropriate node
static void 
process_attributes(cmark_node* node)
{
    if (node->type == NODE_ATTRIBUTE) {
	if (attributes_follow(node->prev)) {
	    node->prev->attr = node;
	    node->type = CMARK_NODE_NONE; /* so nothing gets output */
	    if (cmark_verbose) fprintf(stderr, "Attaching: [%s] to prev:%s (parent=%s)\n",
		    cmark_chunk_to_cstr(&node->as.literal),
		    cmark_node_get_type_string(node->prev),
		    cmark_node_get_type_string(node->parent));
	} else if (attributes_under(node->parent)) {
	    // special case of it being FIRST node in a paragraph.  In that case, attached to prev element (or parent if there is none) to paragraph
	    cmark_node* ap = node->parent;
	    if ((ap->type == NODE_PARAGRAPH)&&(ap->first_child == node)) {
		if (ap->prev == NULL) ap = ap->parent;
		assert(ap->attr == NULL);
		ap->attr = node;
		// if this para has ONLY the attr, make it be a nop
		if (node->parent->last_child == node) node->parent->type = CMARK_NODE_NONE;
	    } else {
		node->parent->attr = node;
	    }
	    node->type = CMARK_NODE_NONE; /* so nothing gets output */
	    if (cmark_verbose) fprintf(stderr, "Attaching: [%s] to PARENT/PREV:%s (prev=%s)\n",
		    cmark_chunk_to_cstr(&node->as.literal),
		    cmark_node_get_type_string(ap),
		    cmark_node_get_type_string(node->prev));
	} else {
	    if (cmark_verbose) fprintf(stderr, "??????: [%s] to prev:%s (parent=%s)\n",
		    cmark_chunk_to_cstr(&node->as.literal),
		    cmark_node_get_type_string(node->prev),
		    cmark_node_get_type_string(node->parent));
	}
    }

    if (node->next) process_attributes(node->next);
    if (node->first_child) process_attributes(node->first_child);
}


static cmark_node *finalize_document(cmark_parser *parser)
{
    cmark_node *toc;
    while (parser->current != parser->root) {
	if (cmark_verbose) print_tree(parser->current, "WHILE-LOOP-BEFORE");
        parser->current = finalize(parser, parser->current);
	if (cmark_verbose) print_tree(parser->current, "WHILE-LOOP-AFTER");
    }
    if (cmark_verbose) print_tree(parser->root, "POST-WHILE");
    finalize(parser, parser->root);
    if (cmark_verbose) print_tree(parser->root, "POST-FINAL");
    process_inlines(parser->root, parser->refmap, parser->options);
    if (cmark_verbose) print_tree(parser->root, "POST-INLINE");
    process_attributes(parser->root);
    if (cmark_verbose) print_tree(parser->root, "POST-ATTR");
    clean_tree(parser->root);
    if (cmark_verbose) print_tree(parser->root, "POST-CLEAN");
    process_tables(parser->root);
    if (cmark_verbose) print_tree(parser->root, "POST-TABLE");

    /*Add a body in case << syntax was used to include files. This is
     * necessary because the <link> tags to include the files were
     * placed inside a head tag. so we place the rest of the content
     * inside a body tag
     */
    if (parser->root->first_child == parser->root->next) {
	// we didn't find anything
	assert(parser->root->first_child == NULL);
	fprintf(stderr, "Empty input file\n");
	return parser->root;
    }
    parser->root = add_body(parser->root);

    // print out tree as it is now
    {
	cmark_iter *iter = cmark_iter_new(parser->root);
	cmark_node *cur;
	cmark_event_type ev_type;

	if (0) {
	    fprintf(stderr, "==========> Before TOC\n");
	    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
		cur = cmark_iter_get_node(iter);
		
		bool entering = (ev_type == CMARK_EVENT_ENTER);
		
		fprintf(stderr, "%s:%s", entering?"Enter>":"Exit< ", cmark_node_get_type_string(cur));
		if (cur->type == CMARK_NODE_TEXT) 
		    fprintf(stderr, " [%s]", cur->as.literal.data);
		fprintf(stderr, "\n");
	    }
	    fprintf(stderr, "==========< Before TOC\n");
	}
    }

    if((toc = toc_present(parser->root))!=NULL)
	{
	    int maxDepth = atoi(cmark_node_get_user_data(toc));
	    if(maxDepth == -1)
		{
		    maxDepth = 10; //allows upto h10
		}
	    //go to all the valid headers and populate their user_data field
	    add_header_links(parser->root,maxDepth);
	    //add the children to the toc_node as node_items
	    add_toc(toc,parser->root,maxDepth);
	}
    return parser->root;
}

cmark_node *cmark_parse_file(FILE *f, int options)
{
    unsigned char buffer[4096];
    cmark_parser *parser = cmark_parser_new(options);
    size_t bytes;
    cmark_node *document;
    
    while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        bool eof = bytes < sizeof(buffer);
        S_parser_feed(parser, buffer, bytes, eof);
        if (eof) {
            break;
        }
    }
    
    document = cmark_parser_finish(parser);
    cmark_parser_free(parser);
    return document;
}

cmark_node *cmark_parse_document(const char *buffer, size_t len, int options)
{
    cmark_parser *parser = cmark_parser_new(options);
    cmark_node *document;
    
    S_parser_feed(parser, (const unsigned char *)buffer, len, true);
    
    document = cmark_parser_finish(parser);
    cmark_parser_free(parser);
    return document;
}

void
cmark_parser_feed(cmark_parser *parser, const char *buffer, size_t len)
{
    S_parser_feed(parser, (const unsigned char *)buffer, len, false);
}

static void
S_parser_feed(cmark_parser *parser, const unsigned char *buffer, size_t len,
              bool eof)
{
    const unsigned char *end = buffer + len;
    
    while (buffer < end) {
        const unsigned char *eol
	    = (const unsigned char *)memchr(buffer, '\n',
					    end - buffer);
        size_t line_len;
        
        if (eol) {
            line_len = eol + 1 - buffer;
        } else if (eof) {
            line_len = end - buffer;
        } else {
            cmark_strbuf_put(parser->linebuf, buffer, end - buffer);
            break;
        }
        
        //parser->linebuf will be empty unless you have a string of
        //4096 characters without a new line or an end of file. Enter
        //this if statement if you need encountered new line character
        //in second parse
        if (parser->linebuf->size > 0) {
            cmark_strbuf_put(parser->linebuf, buffer, line_len);
            S_process_line(parser, parser->linebuf->ptr,
                           parser->linebuf->size);
            cmark_strbuf_clear(parser->linebuf);
        } else {
            S_process_line(parser, buffer, line_len);
        }
        
        buffer += line_len;
    }
}

static void chop_trailing_hashtags(cmark_chunk *ch)
{
    int n, orig_n;
    
    cmark_chunk_rtrim(ch);
    orig_n = n = ch->len - 1;
    
    // if string ends in space followed by #s, remove these:
    while (n >= 0 && peek_at(ch, n) == '#')
        n--;
    
    // Check for a be a space before the final #s:
    if (n != orig_n && n >= 0 && peek_at(ch, n) == ' ') {
        ch->len = n;
        cmark_chunk_rtrim(ch);
    }
}

static cmark_node*
cmark_parse_curly_block(cmark_chunk *input, int start, cmark_parser *parser)
{
    cmark_chunk depth;
    int matchlen;
    cmark_node_type type;
    int pos;
    int maxDepth = -1;


    if (peek_at(input, start) != '{') return NULL;

    if ((matchlen = scan_toc(input, start))) {
	type = NODE_TOC;
    } else {
	return NULL;
    }
    // we match TOC
    depth = cmark_chunk_dup(input, start, matchlen);
    pos = cmark_chunk_strchr(&depth,':',0);
    if(pos != depth.len)
    {
	maxDepth = (int)(depth.data[pos+1]-'0');
    }
    if ((type == NODE_QUEST)&&(maxDepth == -1)) maxDepth = 1;
    cmark_node *node = make_block(type, parser->line_number, start);
    node->user_data = malloc(sizeof(char)*4);
    sprintf(node->user_data,"%d",maxDepth);
    node->as.qlevel = maxDepth;
    return node;
}

void
failed2match(int line, const char* text, const char* reason)
{
    fprintf(stderr, "Error:%d:Expected %s within '%s'\n", line, reason, text);
  exit(-1);
}

typedef struct {
  const char* start;
  int len;
} Piece;

// parse { X (: Y)* }, return number of pieces.  pieces[maxPieces] = length to final }
static int
parseCurlywidget(const char* text, Piece* pieces, int maxPieces, int line)
{
  assert(maxPieces >= 1);
  int nextPiece = 0;

  int i;
  for (i=0; (text[i] != 0) && (text[i] != '}'); i++) {
    if (text[i] == '{') break;
  }
  if (text[i] != '{') failed2match(line, text, "{");
  i++;
  int nonSpace;			/* first non-space character */
  int foundNonSpace=0;		/* set to 1 on encountering a non-space character */
  for (; (text[i] != 0) && (text[i] != '}'); i++) {
    if (text[i] > ' ') {
      if (!foundNonSpace) {
	// stop skipping spaces
	foundNonSpace = 1;
	nonSpace = i;
      } 
    } else if ((text[i] == ' ')&&foundNonSpace) break;
    if (text[i] == ':') break;
    if (text[i] == '}') break;
  }
  pieces[nextPiece].start = text+nonSpace;
  pieces[nextPiece].len = i-nonSpace;
  nextPiece++;
  while (text[i] == ' ') i++;
  if (text[i] == '}') {
    pieces[nextPiece].len = i+1;
    return nextPiece;
  }
  if (text[i] != ':') failed2match(line, text, ":");
  // now get as many : tokens as there are.  If more than maxPieces, then error
  while (nextPiece < maxPieces) {
    foundNonSpace = 0;
    if (text[i] != ':') break;
    i++;
    for (; (text[i] != 0) && (text[i] != '}'); i++) {
      if (text[i] > ' ') {
	if (!foundNonSpace) {
	  // stop skipping spaces
	  foundNonSpace = 1;
	  nonSpace = i;
	} 
      } else if ((text[i] == ' ')&&foundNonSpace) break;
      if (text[i] == ':') break;
      if (text[i] == '}') break;
    }
    // found a complete token
    pieces[nextPiece].start = text+nonSpace;
    pieces[nextPiece].len = i-nonSpace;
    nextPiece++;
    if (text[i] == '}') {
      pieces[nextPiece].len = i+1;
      return nextPiece;
    }
    if (text[i] != ':') failed2match(line, text, ":");
  }
  // we got here without finding a '}', so error
  failed2match(line, text, "} before run out of max pieces");
  return 0;
}

static int
checkCurlyBlockEnd(cmark_parser* parser, const char*buffer, const char* expect, int dieOnError)
{
    Piece pieces[3];
    int count = parseCurlywidget(buffer, pieces, 2, parser->line_number);
    if (count != 2) {
	if (dieOnError) die("Expected {%s:end} at line %d", expect, parser->line_number);
	return 0;
    }
    // so far, so good
    if ((pieces[1].len != 3)||(strncmp((const char*)(pieces[1].start), "end", 3) != 0)) {
	if (dieOnError) die("Expected {%s:end} at line %d", expect, parser->line_number);
	return 0;
    }
    return 1;
}

// put lines into blocks.

static void
S_process_line(cmark_parser *parser, const unsigned char *buffer, size_t bytes)
{
    cmark_node* last_matched_container;
    int offset = 0;
    int matched = 0;
    int lev = 0;
    int i;
    cmark_list *data = NULL;
    bool all_matched = true;
    cmark_node* container;
    bool blank = false;
    int first_nonspace;
    int indent;
    cmark_chunk input;
    bool maybe_lazy;
    cmark_node* node;	/* used for {toc} and {question} */
    
    //utf8proc_detab will replace tabs with 4 spaces and add the string in buffer to parser->curline
    utf8proc_detab(parser->curline, buffer, bytes);
    // Add a newline to the end if not present:
    // TODO this breaks abstraction:
    if (parser->curline->ptr[parser->curline->size - 1] != '\n') {
        cmark_strbuf_putc(parser->curline, '\n');
    }
    input.data = parser->curline->ptr;
    input.len = parser->curline->size;
    
    // container starts at the document root.
    container = parser->root;
    
    parser->line_number++;
    
    // for each containing node, try to parse the associated line start.
    // bail out on failure:  container will point to the last matching node.
    
    //enter this while loop if you are trying to match the current
    //line to something that was left abruptly at the end of the last
    //line but can continue with the new line like a blockquote or a
    //list marker. Keep backing up till you find a match. Worst case
    //you will go upto document
    
    while (container->last_child && container->last_child->open) {

	if (cmark_verbose) fprintf(stderr, 
			     "----- container searching:%d:%s->%s:[%s]\n",
			     parser->line_number,
			     cmark_node_get_type_string(container),
			     cmark_node_get_type_string(container->last_child),
			     input.data);

        container = container->last_child;
        
        first_nonspace = offset;
        while (peek_at(&input, first_nonspace) == ' ') {
            first_nonspace++;
        }
        
	indent = first_nonspace - offset;
        blank = peek_at(&input, first_nonspace) == '\n';
        
        //blockquote matches if the indent of next line >=3
        if (container->type == NODE_BLOCK_QUOTE) {
            matched = indent <= 3 && peek_at(&input, first_nonspace) == '>';
            if (matched) {
                offset = first_nonspace + 1;
                if (peek_at(&input, offset) == ' ')
                    offset++;
            } else {
                all_matched = false;
            }
            
        } else if (container->type == NODE_ITEM) {
            
            if (indent >= container->as.list.marker_offset +
                container->as.list.padding) {
                offset += container->as.list.marker_offset +
		    container->as.list.padding;
            } else if (blank) {
                offset = first_nonspace;
            } else {
                all_matched = false;
            }
            
        } else if (container->type == NODE_CODE_BLOCK) {
            
            if (!container->as.code.fenced) { // indented
                if (indent >= CODE_INDENT) {
                    offset += CODE_INDENT;
                } else if (blank) {
                    offset = first_nonspace;
                } else {
                    all_matched = false;
                }
            } else { // fenced have ``` or ~~~
                matched = 0;
                if (indent <= 3 &&
                    (peek_at(&input, first_nonspace) ==
                     container->as.code.fence_char)) {
		    matched = scan_close_code_fence(&input,first_nonspace);
		}
                if (matched >= container->as.code.fence_length) {
                    // closing fence - and since we're at
                    // the end of a line, we can return:
                    all_matched = false;
                    offset += matched;
                    parser->current = finalize(parser, container);
                    goto finished;
                } else {
                    // skip opt. spaces of fence offset
                    i = container->as.code.fence_offset;
                    while (i > 0 &&
                           peek_at(&input, offset) == ' ') {
                        offset++;
                        i--;
                    }
                }
            }
        } else if (container->type == NODE_HEADER) {
            
            // a header can never contain more than one line
            all_matched = false;
            
        } else if (container->type == NODE_HTML) {
            
            if (blank) {
                all_matched = false;
            }
            
        } else if (container->type == NODE_PARAGRAPH) {
            if (blank) {
                all_matched = false;
            }
            
        } else if (container->type == NODE_EXAM) {
	    // check to see if we are ending an exam question
	    // while here so we can break out of it
	    if (cmark_verbose) fprintf(stderr, "Checking to see if we should end exam:[%s]\n", input.data);
	    while (1) {
		// we demand that exam starts and ends occur in 1st column
		if (indent != 0) break;
		if (blank) break;

		if (cmark_verbose) fprintf(stderr, "scanning\n");

		if (!scan_question_qwidget(&input, first_nonspace)) break;
	    
		// we matched a question, it better be an end
		Piece pieces[3];
		int count = parseCurlywidget((const char*)(input.data+first_nonspace), pieces, 2, parser->line_number);
		int openType = container->as.examquestion.type;
		if (count != 2) failed2match(parser->line_number, (const char*)input.data, "expected {X:end}");
	    
		if ((pieces[0].len == qtypes[openType].len) && 
		    (strncmp(pieces[0].start, qtypes[openType].str, qtypes[openType].len) == 0)) {      
		    // so far, so good
		    if ((pieces[1].len != 3)||(strncmp((const char*)(pieces[1].start), "end", 3) != 0)) {
			fprintf(stderr, "Expected end at %d\n", parser->line_number);
		    }
		    // ok, go up to parent
		    parser->current = finalize(parser, container);
		    goto finished;
		}
		break;
	    }
	    break;
	} else if (container->type == NODE_SOLUTION) {
	    // check to see if we are ending an exam question
	    // while here so we can break out of it
	    if (cmark_verbose) fprintf(stderr, "Checking to see if we should end solution:[%s]\n", input.data);
	    while (1) {
		// we demand that exam starts and ends occur in 1st column
		if (indent != 0) break;
		if (blank) break;

		if (cmark_verbose) fprintf(stderr, "scanning sol\n");

		if (!scan_solution(&input, first_nonspace)) break;
	    
		// we matched a solution, it better be an end
		checkCurlyBlockEnd(parser, (const char*)(input.data+first_nonspace), "solution", 1);
		// ok, go up to parent
		parser->current = finalize(parser, container);
		goto finished;
	    }
	    break;
	} else if (container->type == NODE_TABLE)  {
	    if (!scan_table(&input, first_nonspace)) break;
	    // we matched a table.  I hope it is an END!
	    checkCurlyBlockEnd(parser, (const char*)(input.data+first_nonspace), "table", 1);
	    // ok, go up to parent
	    parser->current = finalize(parser, container);
	    goto finished;
	}
        
        if (!all_matched) {
            container = container->parent;  // back up to last matching node
            break;
        }
    }
    
    if (cmark_verbose) fprintf(stderr, 
	    "----- Finished container searching:%d:%s^%s:[%s]\n",
	    parser->line_number,
	    cmark_node_get_type_string(container),
	    container->parent ? cmark_node_get_type_string(container->parent) : "TOP",
	    input.data);

    last_matched_container = container;
    // check to see if we've hit 2nd blank line, break out of list:
    if (blank && container->last_line_blank) {
        break_out_of_lists(parser, &container);
    }
    
    maybe_lazy = parser->current->type == NODE_PARAGRAPH;
    // try new container starts:
    while (container->type != NODE_CODE_BLOCK &&
	   container->type != NODE_SOLUTION &&
           container->type != NODE_HTML) {
        //first_nonspace has to be set selectively in each case
        first_nonspace = offset;
        while (peek_at(&input, first_nonspace) == ' ')
            first_nonspace++;
        
        indent = first_nonspace - offset;
        blank = peek_at(&input, first_nonspace) == '\n';
        
        if (indent >= CODE_INDENT) {
            if (!maybe_lazy && !blank) {
                offset += CODE_INDENT;
                container = add_child(parser, container, NODE_CODE_BLOCK, offset + 1);
                container->as.code.fenced = false;
                container->as.code.fence_char = 0;
                container->as.code.fence_length = 0;
                container->as.code.fence_offset = 0;
                container->as.code.info = cmark_chunk_literal("");
            } else { // indent > 4 in lazy line
                break;
            }
            
        } else if (peek_at(&input, first_nonspace) == '>') {
            
            offset = first_nonspace + 1;
            // optional following character
            if (peek_at(&input, offset) == ' ')
                offset++;
            container = add_child(parser, container, NODE_BLOCK_QUOTE, offset + 1);
            
        } else if ((matched = scan_atx_header_start(&input, first_nonspace))) {
            offset = first_nonspace + matched;
            container = add_child(parser, container, NODE_HEADER, offset + 1);
            int hashpos = cmark_chunk_strchr(&input, '#', first_nonspace);
            int level = 0;
            
            while (peek_at(&input, hashpos) == '#') {
                level++;
                hashpos++;
            }
            container->as.header.level = level;
            container->as.header.setext = false;
            
        } else if ((matched = scan_solutionClear(&input, first_nonspace))) {
	    // we have matched a solution clear block, insert into tree, nothing else for now
	    if (cmark_verbose) fprintf(stderr, "Adding solution clear inside (%s)\n", cmark_node_get_type_string(container));
	    container = add_child(parser, container, NODE_CLEAR_SOLUTION, first_nonspace);
	    container->as.solution.stanza = -1; /* indicate clear */
	    offset = first_nonspace+matched;
	} else if ((matched = scan_open_code_fence(&input, first_nonspace))) {
            
            container = add_child(parser, container, NODE_CODE_BLOCK, first_nonspace + 1);
            container->as.code.fenced = true;
            container->as.code.fence_char = peek_at(&input, first_nonspace);
            container->as.code.fence_length = matched;
            container->as.code.fence_offset = first_nonspace - offset;
            container->as.code.info = cmark_chunk_literal("");
            offset = first_nonspace + matched;
            
        } else if ((matched = scan_table(&input, first_nonspace))) {
	    // parse table
	    if (cmark_verbose) fprintf(stderr, "Adding table inside (%s)\n", cmark_node_get_type_string(container));
            container = add_child(parser, container, NODE_TABLE, first_nonspace);
	    Piece pieces[5];
	    int count = parseCurlywidget((const char*)(input.data+first_nonspace), pieces, 5, parser->line_number);
	    int i;
	    if (0) {
		for (i=0; i<count; i++) {
		    char buffer[128];
		    strncpy(buffer, pieces[i].start, pieces[i].len);
		    buffer[pieces[i].len] = 0;
		    fprintf(stderr, "%d [%s] %d\n", i, buffer, pieces[i].len);
		}
		fprintf(stderr, "%d [ENTIRE] %d\n", i, pieces[i].len);
	    }

	    if (count < 2) failed2match(parser->line_number, (const char*)input.data, "expected {table:begin:...}");
	    if ((pieces[1].len != 5)||(strncmp((const char*)(pieces[1].start), "begin", 5) != 0)) {
		// how to deal with error?
		char buffer[128];
		int len = pieces[1].len;
		if (len > 100) len=100;
		strncpy(buffer, (const char*)pieces[1].start, len);
		if (len != pieces[1].len) strcpy(buffer+100, "..."); else buffer[len] = 0;
		fprintf(stderr, "Error:%d: Unexpected table '%s'.  Missing begin?\n", parser->line_number, buffer);
		exit(-1);
	    }

	    char buffer[128];
	    strncpy(buffer, (const char*)pieces[2].start, pieces[2].len);
	    buffer[pieces[2].len] = 0;
	    int cols = atoi(buffer);
	    if ((cols<1)||(cols>20)) {
		fprintf(stderr, "Expected cols spec at line %d, instead found [%s]\n", parser->line_number, buffer);
		exit(-1);
	    }
	    container->as.table.cols = cols;

	    if (count == 3) {
		if ((pieces[3].len < 1)||(pieces[3].len>4)) {
		    fprintf(stderr, "Expected cols separator string\n");
		    exit(-1);
		}
		strncpy(container->as.table.sep, pieces[3].start, pieces[3].len);
	    } else {
		strcpy(container->as.table.sep, "|");
	    }

	    offset = first_nonspace + pieces[count].len;
        } else if ((matched = scan_question_qwidget(&input, first_nonspace))) {
	    // we got a {begin} tag to start a widget for an answer
	    if (cmark_verbose) fprintf(stderr, "Adding exam inside (%s)\n", cmark_node_get_type_string(container));
	    container = add_child(parser, container, NODE_EXAM, first_nonspace);
	    // lets figure out which kind
	    Piece pieces[3];
	    int count = parseCurlywidget((const char*)(input.data+first_nonspace), pieces, 2, parser->line_number);
	    int i;
	    if (count != 2) failed2match(parser->line_number, (const char*)input.data, "expected {X:begin}");
	    for (i=0; qtypes[i].len != 0; i++) {
		if (cmark_verbose) fprintf(stderr, "Checking %d %s of %d\n", i, qtypes[i].str, qtypes[i].len);
		if ((pieces[0].len == qtypes[i].len) && (strncmp(pieces[0].start, qtypes[i].str, qtypes[i].len) == 0)) {
		    container->as.examquestion.type = qtypes[i].type;
		    // needs to be a begin
		    if ((pieces[1].len != 5)||(strncmp((const char*)(pieces[1].start), "begin", 5) != 0)) {
			// how to deal with error?
			fprintf(stderr, "Expected begin at %d\n", parser->line_number);
		    }
		    break;
		}
	    }
	    assert(qtypes[i].len != 0);
	    offset = first_nonspace + pieces[2].len;
	    if (cmark_verbose) fprintf(stderr, "offset %d=%d+%d, %d [%s]\n", offset, first_nonspace, pieces[2].len, 
		    (int)strlen((const char*)input.data), input.data);
	} else if ((matched = scan_solution(&input, first_nonspace))) {
	    // we got a {begin} tag to start a solution
	    if (cmark_verbose) fprintf(stderr, "Adding solution inside (%s)\n", cmark_node_get_type_string(container));
	    container = add_child(parser, container, NODE_SOLUTION, first_nonspace);
	    // lets figure out which kind
	    Piece pieces[5];
	    int count = parseCurlywidget((const char*)(input.data+first_nonspace), pieces, 3, parser->line_number);
	    if (count < 2) failed2match(parser->line_number, (const char*)input.data, "expected {X:begin}");
	    int len;
	    if (count == 2) {
		len = pieces[2].len;
		// default to stanza 1
		container->as.solution.stanza = 1;
	    } else if (count == 3) {
		len = pieces[3].len;
		char numstr[128];
		int nlen = pieces[2].len;
		if (nlen>120) nlen = 120;
		strncpy(numstr, pieces[2].start, nlen);
		numstr[nlen] = 0;
		int stanzaNumber = atoi(numstr);
		if ((stanzaNumber < 1)||(stanzaNumber > 2000))
		    die("Expected a stanzaNumber for {solution} at line %d to be between 1 and 2000, not %d", parser->line_number, stanzaNumber);
		container->as.solution.stanza = stanzaNumber;
	    } else {
		die("Malformed {solution} start at line %d", parser->line_number);
	    }
	    offset = first_nonspace + len;
	    if (cmark_verbose) fprintf(stderr, "solution offset %d=%d+%d, %d [%s]\n", offset, first_nonspace, pieces[2].len, 
		    (int)strlen((const char*)input.data), input.data);
	} else if ((matched = scan_html_block_tag(&input, first_nonspace))) {
            
            container = add_child(parser, container, NODE_HTML, first_nonspace + 1);
            // note, we don't adjust offset because the tag is part of the text
            // a setext header is one of the form
            // header
            //-----/======
            //previously encountered the text so now parse the ==== and set the header fields
        } else if (container->type == NODE_PARAGRAPH &&
                   (lev = scan_setext_header_line(&input, first_nonspace)) &&
                   // check that there is only one line in the paragraph:
                   cmark_strbuf_strrchr(&container->string_content, '\n',cmark_strbuf_len(&container->string_content) - 2) < 0) {
	    //because header can contain only 1 line
	    container->type = NODE_HEADER;
	    container->as.header.level = lev;
	    container->as.header.setext = true;
	    offset = input.len - 1;
            
	} else if (!(container->type == NODE_PARAGRAPH && !all_matched) &&
		   (matched = scan_hrule(&input, first_nonspace))) {
                       
	    // it's only now that we know the line is not part of a setext header:
	    container = add_child(parser, container, NODE_HRULE, first_nonspace + 1);
	    container = finalize(parser, container);
	    offset = input.len - 1;
                       
	} else if ((matched = parse_list_marker(&input, first_nonspace, &data))) {
                       
	    // compute padding:
	    offset = first_nonspace + matched;
	    i = 0;
	    while (i <= 5 && peek_at(&input, offset + i) == ' ') {
		i++;
	    }
	    // i = number of spaces after marker, up to 5
	    if (i >= 5 || i < 1 || peek_at(&input, offset) == '\n') {
		data->padding = matched + 1;
		if (i > 0) {
		    offset += 1;
		}
	    } else {
		data->padding = matched + i;
		offset += i;
	    }
                       
	    // check container; if it's a list, see if this list item
	    // can continue the list; otherwise, create a list container.
                       
	    data->marker_offset = indent;
                       
	    if (container->type != NODE_LIST ||
		!lists_match(&container->as.list, data)) {
		container = add_child(parser, container, NODE_LIST,first_nonspace + 1);
                           
		memcpy(&container->as.list, data, sizeof(*data));
	    }
                       
	    // add the list item
	    container = add_child(parser, container, NODE_ITEM,first_nonspace + 1);
	    /* TODO: static */
	    memcpy(&container->as.list, data, sizeof(*data));
	    free(data);
	} else if (!(container->type == NODE_PARAGRAPH && !all_matched) &&
		   (node = cmark_parse_curly_block(&input, first_nonspace, parser))) {
                       
	    //fprintf(stderr, "Found {%s:%d}\n", cmark_node_get_type_string(node), node->as.qlevel);

	    // we have a {toc} add it in
	    add_already_created_child(parser, container, node);
	    container = finalize(parser, node);
	    offset = input.len - 1;
	} else {
	    break;
	}
        
        if (accepts_lines(container->type)) {
            // if it's a line container, it can't contain other containers
            break;
        }
        maybe_lazy = false;
    }
    
    // what remains at offset is a text line.  add the text to the
    // appropriate container.
    
    first_nonspace = offset;
    while (peek_at(&input, first_nonspace) == ' ')
        first_nonspace++;
    
    indent = first_nonspace - offset;
    blank = peek_at(&input, first_nonspace) == '\n';
    
    if (blank && container->last_child) {
        container->last_child->last_line_blank = true;
    }
    
    // block quote lines are never blank as they start with >
    // and we don't count blanks in fenced code for purposes of tight/loose
    // lists or breaking out of lists.  we also don't set last_line_blank
    // on an empty list item.
    container->last_line_blank = (blank &&
                                  container->type != NODE_BLOCK_QUOTE &&
                                  container->type != NODE_HEADER &&
				  container->type != NODE_SOLUTION &&
                                  !(container->type == NODE_CODE_BLOCK &&
                                    container->as.code.fenced) &&
                                  !(container->type == NODE_ITEM &&
                                    container->first_child == NULL &&
                                    container->start_line == parser->line_number));
    
    cmark_node *cont = container;
    while (cont->parent) {
        cont->parent->last_line_blank = false;
        cont = cont->parent;
    }
    //multiple lines in a paragraph initially all get added to the strbuf in the paragraph node
    if (parser->current != last_matched_container &&
        container == last_matched_container &&
        !blank &&
        parser->current->type == NODE_PARAGRAPH &&
        cmark_strbuf_len(&parser->current->string_content) > 0) {
        add_line(parser->current, &input, offset);
        
    } else { // not a lazy continuation
        // finalize any blocks that were not matched and set cur to container:
        while (parser->current != last_matched_container) {
            parser->current = finalize(parser, parser->current);
            assert(parser->current != NULL);
        }
        
        if (container->type == NODE_CODE_BLOCK ||
	    container->type == NODE_SOLUTION ||
            container->type == NODE_HTML) {
            
            add_line(container, &input, offset);
            
        } else if (blank) {
            // ??? do nothing
            
        } else if (accepts_lines(container->type)) {
            //multiple lines in a paragraph initially all get added to the strbuf in the paragraph node
            if (container->type == NODE_HEADER &&
                container->as.header.setext == false) {
                chop_trailing_hashtags(&input);
            }
            add_line(container, &input, first_nonspace);
            
        } else {
            // create paragraph container for line
            container = add_child(parser, container, NODE_PARAGRAPH, first_nonspace + 1);
            add_line(container, &input, first_nonspace);
        }
        parser->current = container;
    }
 finished:
    parser->last_line_length = parser->curline->size -
	(parser->curline->ptr[parser->curline->size - 1] == '\n' ?
	 1 : 0);
    ;
    cmark_strbuf_clear(parser->curline);
}

/* This function finds the first node in the AST that is not an include tag ( a tag that includes files using <<). This is necessary to separate the document into a head and a body */
cmark_node *find_first_non_include(cmark_node *document)
{
    cmark_iter *iter = cmark_iter_new(document);
    cmark_event_type ev_type;
    while((ev_type = cmark_iter_next(iter))!=CMARK_EVENT_DONE)
    {
        if(ev_type==CMARK_EVENT_ENTER)
        {
            cmark_node *node = cmark_iter_get_node(iter);
            if(node->type!=CMARK_NODE_INCLUDE && node->type!=CMARK_NODE_DOCUMENT)
            {
                return node;
            }
        }
    }
    return NULL;
}

/* Thus function takes a node of type NODE_DOCUMENT and the name of
 * the file to include as specified by the << tag and creates a new
 * NODE_INCLUDE and adds it as a child to the head tag, taking care to
 * create/use a head if/if it doesn't exist */
void cmark_add_to_head(cmark_node *node,char *filename)
{
    if(node->type!=NODE_DOCUMENT)
    {
        fprintf(stderr,"Head can only be added to document node \n");
        exit(1);
    }
    assert(node->type==NODE_DOCUMENT);
    cmark_node *new_include = cmark_node_new(NODE_INCLUDE);
    if(!cmark_node_set_literal(new_include,filename))
    {
        fprintf(stderr,"could not set literal \n");
        exit(1);
    }
    if(node->first_child->type!=NODE_HEAD)
    {
        cmark_node_prepend_child(node,cmark_node_new(NODE_HEAD));
        cmark_node_append_child(node->first_child,new_include);
    }
    else
    {
        cmark_node_append_child(node->first_child,new_include);
    }

}

// This function adds the tags to the head if they were passed in as command line parameters
void cmark_include_files(cmark_node *document,char **argv, int *includes, int numincludes)
{
    for(int i=0;i<numincludes;i++)
    {
        cmark_add_to_head(document,argv[includes[i]]);
    }
}

// Useful debugging function to print the nodes of a tree. I USED THIS FUNCTION A LOT WHILE DEBUGGING
void print_nodes(cmark_node *root)
{
    cmark_event_type ev_type;
    cmark_iter *iter = cmark_iter_new(root);
    while((ev_type = cmark_iter_next(iter))!=CMARK_EVENT_DONE)
    {
        cmark_node *cur = cmark_iter_get_node(iter);
        printf("Node is of type %s and event_type = %d \n",cmark_node_get_type_string(cur),ev_type);
        if(cur->type==NODE_INCLUDE)
            printf("%s \n",cmark_node_get_literal(cur));
    }
    
    cmark_iter_free(iter);
}

cmark_node *cmark_parser_finish(cmark_parser *parser)
{
    if (parser->linebuf->size) {
        S_process_line(parser, parser->linebuf->ptr,
                       parser->linebuf->size);
        cmark_strbuf_clear(parser->linebuf);
    }
    finalize_document(parser);
    if (parser->options & CMARK_OPT_NORMALIZE) {
        cmark_consolidate_text_nodes(parser->root);
    }
    
    cmark_strbuf_free(parser->curline);
    
#if CMARK_DEBUG_NODES
    if (cmark_node_check(parser->root, stderr)) {
        abort();
    }
#endif
    return parser->root;
}

// Local Variables:
// mode: c           
// c-basic-offset: 4
// End:
