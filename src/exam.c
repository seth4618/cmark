#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include "cmark.h"
#include "node.h"
#include "houdini.h"
#include "exam.h"

// search up to find the first parent which is an exam node
cmark_node*
get_enclosing_exam(cmark_node* node)
{
    while (node && (node->type != CMARK_NODE_EXAM) && node->parent) {
	node = node->parent;
    }
    if (node == NULL) {
	fprintf(stderr, "No enclosing exam node\n");
	exit(-1);
    }
    return node;
}

static void
warning(cmark_node* node, const char* fmt, ...)
{
    char buffer[1024];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(buffer, fmt, ap);
    va_end(ap);
    int line = cmark_node_get_start_line(node);
    int col = cmark_node_get_start_column(node);
    cmark_node* linesrc = node;
    while ((linesrc != NULL) && (line == 0)) {
	linesrc = linesrc->parent;
	line = cmark_node_get_start_line(linesrc);
	col = cmark_node_get_end_line(linesrc);
    }
    
    if (node != NULL) {
	if (node == linesrc) {
	    fprintf(stderr, "Warning:%d:%d %s\n", line, col, buffer);
	} else {
	    if (line == col) {
		fprintf(stderr, "Warning:%d: %s\n", line, buffer);
	    } else {
		fprintf(stderr, "Warning:(between lines %d--%d): %s\n", line, col, buffer);
	    }
	}
    } else {
	fprintf(stderr, "Warning: %s\n", buffer);
    }
}

// each time we hit a solution clear we create a new set of stanza's.  Then at the end we generate all combinations foreach set of clears.
typedef struct {
    int numOutputs;
    cmark_strbuf** outputs;
    int* outsUsed;
    char** cleanOutputs;
    int count;
} StanzaSet;
#define MaxSolutionClears 10
static int numSolutionClears = 0;
static StanzaSet solutionClearList[MaxSolutionClears];


// between each clearlist this tracks the outputs for each stanza
static int numExamOutputs = 0;
static cmark_strbuf** examOutputs = NULL;
static int* examOutUsed = NULL;
static int useExamOutput = 0;

const char* qs[] = { "radio", "check", "matchleft", "matchright", "text" };
static int uniqID = 0;

Qtype* qtypes = NULL;

void
installQtypes(void) {
  if (qtypes != NULL) return;

  qtypes = (Qtype*)calloc(Q_MAXWIDGET+2, sizeof(Qtype));
  for (int i=0; i<(Q_MAXWIDGET+1); i++) {
    qtypes[i].len = strlen(qs[i]);
    qtypes[i].str = qs[i];
    qtypes[i].type = i;
  }
  qtypes[Q_MAXWIDGET+1].len = 0;
}

typedef struct {
    int num;
    char type;
    int ansnum;
} Qcounter;

#define MAX_Q_LEVEL 10

extern Qcounter questions[MAX_Q_LEVEL];


Qcounter questions[MAX_Q_LEVEL];

static char types[] = { '1', 'A', '1', 'a', '1', 'A', '1', 'a', '1', 'A', '1', 'a' };

static int qid = 0;

// init all the counters, for now predefined formats
void 
initQuestionCounter(void)
{
    for (int i=0; i<MAX_Q_LEVEL; i++) {
	questions[i].num = 1;
	questions[i].type = types[i];
	questions[i].ansnum = 0;
    }
}

static char* roman[] = {
    "?",
    "i",
    "ii",
    "iii",
    "iv",
    "v",
    "vi",
    "vii",
    "viii",
    "ix",
    "x"
};
    
static void
format(char* bp, int ctr, char type)
{
    assert(ctr > 0);
    switch(type) {
    case '1':
	sprintf(bp, "%d", ctr);
	break;

    case 'A':
	assert(ctr<26);
	*bp++ = 'A'+(ctr-1);
	*bp = 0;
	break;

    case 'a':
	assert(ctr<26);
	*bp++ = 'a'+(ctr-1);
	*bp = 0;
	break;

    case 'i':
	assert(ctr <= 9);
	strcpy(bp, roman[ctr]);
	break;

    default:
	assert(0);
    }
}

static int lastQlevel = 0;

// get the current counter, formated properly, for the level indicated and increment it
char* 
updateAndFormatQuestion(int level, cmark_node* node)
{
    static char buffer[128];
    char* bp;
    lastQlevel = level-1;

    bp = &(buffer[0]);
    *bp = 0;

    for (int i=0; i<level-1; i++) {
	if (questions[i].num-1 == 0) {
	    warning(node, "Have a level %d question without a level %d question",
		    i+2, i+1);
	} else {
	    format(bp, questions[i].num-1, questions[i].type);
	    strcat(bp, ".");
	    bp += strlen(bp);
	}
    }
    format(bp, questions[level-1].num, questions[level-1].type);
    questions[level-1].num++;
    questions[level-1].ansnum = 0;
    for (int i=level; i<MAX_Q_LEVEL; i++) {
	questions[i].num = 1;
	questions[i].ansnum = 0;
    }
    return buffer;
}

static int
getGroupNumAtQuestionLevel(void)
{
    int groupnum = 0;
    for (int i=0; i<=lastQlevel; i++) {
	groupnum = groupnum*10 + questions[i].num;
    }
    return groupnum;
}

static int 
getAnsnumAtQuestionLevel(void)
{
    return questions[lastQlevel].ansnum++;
}

static cmark_strbuf*
getExamOutputPtr(int stanzaNumber)
{
    assert((stanzaNumber >= 0)&&(stanzaNumber < 100));
    if (stanzaNumber+1 >= numExamOutputs) {
	int newCount = (numExamOutputs == 0) ? 4 : numExamOutputs*2;
	examOutputs = (cmark_strbuf**)realloc(examOutputs, newCount * sizeof(cmark_strbuf*));
	examOutUsed = (int*)realloc(examOutUsed, newCount * sizeof(int));
	int i;
	for (i = numExamOutputs; i<newCount; i++) {
	    examOutUsed[i] = 0;
	    examOutputs[i] = (cmark_strbuf*)malloc(sizeof(cmark_strbuf));
	    cmark_strbuf_init(examOutputs[i], 256);
	}
	numExamOutputs = newCount;
    }
    examOutUsed[stanzaNumber] = 1;
    return examOutputs[stanzaNumber];
}

// whatever we have gotten so far is to be combined with what comes next in an all pairs mode.
// so if we have 3 solution blocks before a clear and 2 after, we will generate 6 solution blocks.
void 
clearSolutionBlock(cmark_strbuf *outputBuffer, cmark_node* node, int entering)
{
    if (!entering) return;

    if (outputBuffer) cmark_strbuf_puts(outputBuffer, "\n<!-- solution clear -->\n");

    if (numSolutionClears >= (MaxSolutionClears-2)) {
	fprintf(stderr, "Too many solution clears.\n");
	exit(-1);
    }
    // save current set of buffers and start over
    solutionClearList[numSolutionClears].numOutputs = numExamOutputs;
    solutionClearList[numSolutionClears].outputs = examOutputs;
    solutionClearList[numSolutionClears].outsUsed = examOutUsed;
    numSolutionClears++;

    examOutputs = NULL;
    examOutUsed = NULL;
    numExamOutputs = 0;
}

void
markSolutionBlock(cmark_strbuf *outputBuffer, cmark_node* node, int entering)
{
    if (!entering) return;

    cmark_strbuf_puts(outputBuffer, "\n<!-- starting solution block ");

    cmark_strbuf* solutionBuffer = outputBuffer;
    if (useExamOutput != 0) {
	int stanzaNumber = node->as.solution.stanza;
	solutionBuffer = getExamOutputPtr(stanzaNumber); 
    }
    int len = node->as.solution.literal.len;
    if (len < 0) len = strlen((char*)node->as.solution.literal.data);
    houdini_escape_html0(solutionBuffer, node->as.solution.literal.data, (size_t)len, 0);

    cmark_strbuf_puts(outputBuffer, " Ending solution block -->\n");
}


char* 
examQuestion2str(cmark_node* node)
{
  static char buffer[128];
  sprintf(buffer, "<examq:%s>", qs[node->as.examquestion.type]);
  return buffer;
}

void
assignExamNumber(cmark_node* node)
{
    node->as.examquestion.group = uniqID++;
    node->as.examquestion.ansnum = 0;
}

void
format_text_wiget(cmark_strbuf *html, int width, int groupnum, int ansnum)
{
    if (groupnum < 0) {
	// this is a blank, so use question level numbering
	groupnum = getGroupNumAtQuestionLevel();
	ansnum = getAnsnumAtQuestionLevel();
    }

   cmark_strbuf_printf(html, "<input type=\"text\" id=\"Q%d\" name=\"text%d%d\" size=\"%d\">",
		       qid++, groupnum, ansnum, width);
}

void
format_blank_input(cmark_strbuf *html, cmark_node* node, int groupnum, int ansnum)
{
    char* istr = (char*)node->user_data;
    int width = 10;
    if (strlen(istr) > 0) {
	width = atoi(istr);
	if ((width < 1)||(width > 40)) {
	    fprintf(stderr, "Illegal width [%s]\n", istr);
	    exit(-1);
	}
    }
    format_text_wiget(html, width, groupnum, ansnum);
}


void
format_exam_widget(cmark_strbuf *html, cmark_node* qtype, cmark_node* node)
{
    int groupnum = qtype->as.examquestion.group;
    int ansnum = qtype->as.examquestion.ansnum++;
    int type = qtype->as.examquestion.type;

    switch (type) {
    case Q_RADIO: {
	char* val = (char*)node->user_data;
	char buffer[128];
	if (strlen(val) == 0) {
	    sprintf(buffer, "ans%d", ansnum);
	    val = buffer;
	}
	cmark_strbuf_printf(html, "<input type=\"radio\" name=\"radio%d\" value=\"%s\">",
			    groupnum,
			    val);
        }
	break;

    case Q_CHECK: {
	char* val = (char*)node->user_data;
	char buffer[128];
	if (strlen(val) == 0) {
	    sprintf(buffer, "ans%d", ansnum);
	    val = buffer;
	}
	cmark_strbuf_printf(html, "<input type=\"checkbox\" name=\"check%d\" value=\"%s\">",
			    groupnum,
			    val);
    	}
	break;

    case Q_TEXT:
	format_blank_input(html, node, groupnum, ansnum);
	break;

    case Q_LEFT:
    case Q_RIGHT:
    default:
	assert(0);
    }
}

static int numberOfFillIns = 0;
static cmark_strbuf* questionDescriptorBuffer;

static void 
outQinfo(cmark_strbuf* solutionBuffer, const char* string)
{
    if (numberOfFillIns > 0) cmark_strbuf_putc(solutionBuffer, ',');
    cmark_strbuf_puts(solutionBuffer, string);
    numberOfFillIns++;
}

// enable exam output, as well as describe the types of each question
void
create_exam_output_buffer(cmark_node* root)
{
    useExamOutput = 1;
    cmark_strbuf* solutionBuffer = (cmark_strbuf*)malloc(sizeof(cmark_strbuf));
    cmark_strbuf_init(solutionBuffer, 256);
    cmark_iter *iter = cmark_iter_new(root);

    cmark_node* group = NULL;
    int grouptype = 0;
    cmark_event_type ev_type;
    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
	cmark_node* cur = cmark_iter_get_node(iter);
	bool entering = (ev_type == CMARK_EVENT_ENTER);
	switch (cur->type) {
	case CMARK_NODE_EXAM:
	    if (entering) {
		group = cur;
		grouptype = cur->as.examquestion.type;
		if ((grouptype == Q_RADIO)||(grouptype == Q_CHECK)) {
		    outQinfo(solutionBuffer, qs[grouptype]);
		}
	    } else {
		group = NULL;
		grouptype = -1;
	    }
	    break;

	case CMARK_NODE_ANSWER:
	    if (entering) {
		cmark_node* qtype = get_enclosing_exam(cur);
		if (qtype != group) {
		    warning(cur, "{answer} node surrounded by a {text|radio|check:begin} and {...:end} pair?");
		}
		if (grouptype == Q_TEXT) {
		    outQinfo(solutionBuffer, "text");
		}
	    }
	    break;
		
	case CMARK_NODE_BLANK:
	    if (entering) {
		if ((group != NULL)&&(grouptype != Q_TEXT)) {
		    warning(cur, "blank inside a different kind of exam set?");
		}
		outQinfo(solutionBuffer, "text");
	    }
	    break;

	case CMARK_NODE_CODE_BLOCK:
	    if (!entering) break;
	    // look into the literal and check for occurences of {blank}
	    for (const char *p = (const char*)cur->as.code.literal.data; *p; p++) {
		p = strstr(p, "{blank");
		if (p == NULL) break;
		p += 6;
		while (*p==' ') p++;
		if (*p == ':') {
		    p++;
		    while (((*p>='0') && (*p<='9'))||(*p==' ')) p++;
		} 
		while (*p==' ') p++;
		if (*p == '}') {
		    outQinfo(solutionBuffer, "text");
		} 
		p--;
	    }
	    break;

	default:
	    break;
	}
    }
    cmark_iter_free(iter);
    cmark_strbuf_putc(solutionBuffer, '\n');
    fprintf(stderr, "There are %d things to do for this question\n", numberOfFillIns);
    questionDescriptorBuffer = solutionBuffer;
}

static int
cntLines(char* buffer)
{
    int nl = 1;
    char* p = buffer;
    while (*p++) if (*p == '\n') nl++;
    return nl;
}

static char*
getCleanBuffer(cmark_strbuf* buffer)
{
    char* result = (char *)cmark_strbuf_detach(buffer);
    char* clean = result;
    while (*clean) {
	if ((*clean == ' ')||(*clean == '\t')||(*clean == '\n')) {
	    clean++;
	    continue;
	}
	break;
    }
    char* p = clean+strlen(clean)-1;
    while (p > clean) {
	if ((*p == ' ')||(*p == '\t')||(*p == '\n')) {
	    p--;
	    continue;
	}
	break;
    }
    *(p+1) = 0;
    return clean;
}

static void
outCombinedSolution(FILE* out, int* selection, int level)
{
    if (level == numSolutionClears) {
	for (int i=0; i<numSolutionClears; i++) 
	    fprintf(out, "%s\n", solutionClearList[i].cleanOutputs[selection[i]]);
	fprintf(out, "\n");
	return;
    }
    for (int i=0; i<solutionClearList[level].count; i++) {
	selection[level] = i;
	outCombinedSolution(out, selection, level+1);
    }
}

int
cmark_write_exam_output(FILE* out)
{
    // write out descriptor
    fprintf(out, "%s\n", (char *)cmark_strbuf_detach(questionDescriptorBuffer));
    
    // lets clean up each of the stanzaSets, check that each set of
    // stanzas has same line count.  Then, output by doing a full
    // cross product.
    clearSolutionBlock(NULL, NULL, 1);

    int mismatch = 0;
    for (int i=0; i<numSolutionClears; i++) {
	StanzaSet* set = &(solutionClearList[i]);
	// count number of stanzas in this block of solutions
	int cnt = 0;
	for (int j=0; j<set->numOutputs; j++) {
	    if (set->outsUsed[j] == 1) cnt++;
	}
	set->cleanOutputs = calloc(cnt, sizeof(char*));
	set->count = cnt;
	// now convert buffers into clean strings
	cnt = 0;
	for (int j=0; j<set->numOutputs; j++) {
	    if (set->outsUsed[j] == 1) 
		set->cleanOutputs[cnt++] = getCleanBuffer(set->outputs[j]);
	}
	if (cnt == 0) {
	    fprintf(stderr, "No lines in solution block(s)\n");
	    mismatch++;
	    break;
	}
	// ok, now we have cnt strings in cleanOutputs.  Lets make sure all have same count
	int ansCount = cntLines(set->cleanOutputs[0]);
	for (int k=0; k<cnt; k++) {
	    if (ansCount != cntLines(set->cleanOutputs[k])) {
		fprintf(stderr, 
			"In the %dth out of %d stanzasets there was a mismatch with %d answers\n", 
			k, cnt, cntLines(set->cleanOutputs[k]));
		mismatch++;
	    }
	}
    }
    if (mismatch != 0) {
	fprintf(stderr, "Please fix solution blocks\n");
	return -1;
    }
    // now that we have clean strings, lets make sure the total for
    // all sets equals number of questions.
    int totalAnswers = 0;
    for (int i=0; i<numSolutionClears; i++) {
	StanzaSet* set = &(solutionClearList[i]);
	totalAnswers += cntLines(set->cleanOutputs[0]);
    }
    if (totalAnswers != numberOfFillIns) {
	fprintf(stderr, "# of lines for all solution blocks is %d, but expected %d\n", totalAnswers, numberOfFillIns);
	exit(-1);
    }

    // ok, we have a set of good clear strings.  Combine all possible ways
    int* selection = calloc(numSolutionClears, sizeof(int));
    outCombinedSolution(out, selection, 0);
    if (mismatch) return 1;
    return 0;
}


char*
cmark_get_exam_output(int x)
{
    char *result;
    assert (x < numExamOutputs);
    if (examOutUsed[x] == 0) {
	result = calloc(10, sizeof(char));
	return result;
    }
    result = (char *)cmark_strbuf_detach(examOutputs[x]);
    return result;
}

int
needsID(cmark_node* node)
{
    switch (node->as.examquestion.type) {
    case Q_RADIO:
    case Q_CHECK:
	return 1;

    case Q_TEXT:
	return 0;

    default:
	assert(0);
    }
    return 0;
}

// assign id to form if no inputs inside.
void
outExamBlock(cmark_strbuf *html, cmark_node* node, int entering)
{
    char idstring[128];
    idstring[0] = 0;

    if(entering) {
	// assign numner
	if (needsID(node)) {
	    sprintf(idstring, " ID=\"Q%d\" ", qid++);
	}
	assignExamNumber(node);
	cmark_strbuf_printf(html,"<form %s><!-- %d: ", idstring, node->as.examquestion.group);
	cmark_strbuf_puts(html, examQuestion2str(node));
	cmark_strbuf_puts(html,"-->\n");
    } else {
	cmark_strbuf_puts(html,"</form><!-- ");
	cmark_strbuf_puts(html, examQuestion2str(node));
	cmark_strbuf_puts(html,"-->\n");
    }
}



// Local Variables:
// mode: c           
// c-basic-offset: 4
// End:

