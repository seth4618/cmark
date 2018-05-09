#ifndef EXAM_H
#define EXAM_H

#include "buffer.h"
#include "quest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define Q_RADIO 0
#define Q_CHECK 1
#define Q_LEFT 2
#define Q_RIGHT 3
#define Q_TEXT 4
#define Q_NUMBER 5
#define Q_MAXWIDGET 5		/* if you add a new qtype, make sure to reset maxwidget */

typedef struct {
  int len;
  const char* str;
  int type;
} Qtype;

extern Qtype* qtypes;

typedef struct {
    int type;
    int group;			/* uniq ID assigned to this question */
    int ansnum;			/* uniq ID for individual choices in question */
    int size;
} ExamQuestion;

void installQtypes(void);
char* examQuestion2str(cmark_node* node);
void assignExamNumber(cmark_node* node);
void format_exam_widget(cmark_strbuf *html, cmark_node* qtype, cmark_node* node);
    void create_exam_output_buffer(cmark_node* root);
char* cmark_get_exam_output(int x);
int cmark_write_exam_output(FILE* out);
void markSolutionBlock(cmark_strbuf *outputBuffer, cmark_node* node, int entering);
void clearSolutionBlock(cmark_strbuf *outputBuffer, cmark_node* node, int entering);
void format_blank_input(cmark_strbuf *html, cmark_node* node, int groupnum, int ansnum);
void format_text_wiget(cmark_strbuf *html, int width, int groupnum, int ansnum);
cmark_node* get_enclosing_exam(cmark_node* node);

#ifdef __cplusplus
}
#endif

#endif

// Local Variables:
// mode: c           
// c-basic-offset: 4
// End:
