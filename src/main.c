#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "config.h"
#include "cmark.h"
#include "debug.h"
#include "bench.h"
#include "quest.h"
#include "exam.h"

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <io.h>
#include <fcntl.h>
static int nodos = 0;		/* default is allow CR for DOS */
#else
static int nodos = 1;		/* default is no CR for unix */
#endif

extern int cmark_verbose;

static int deleteOnWarning = 0;

static char* outputDir = NULL;

typedef enum {
	FORMAT_NONE,
	FORMAT_HTML,
	FORMAT_XML,
	FORMAT_MAN,
	FORMAT_COMMONMARK
} writer_format;

int can_include(char *filename)
{
    if(strstr(filename,".css")||strstr(filename,".js"))
        return 1;
    return 0;
}

void print_usage()
{
	printf("Usage:   cmark [FILE*]\n");
	printf("Options:\n");
	printf("  --to, -t FORMAT  Specify output format (html, xml, man, commonmark, exam)\n");
	printf("  --width WIDTH    Specify wrap width (default 0 = nowrap)\n");
	printf("  --sourcepos      Include source position attribute\n");
	printf("  --hardbreaks     Treat newlines as hard line breaks\n");
	printf("  --outdir         path to output files (esp useful for exam FORMAT)\n");
	printf("  --dosok          allow CR in the input files (when running on unix)\n");
	printf("  --notstdout      don't write to stdout (esp useful for exam FORMAT)\n");
	printf("  --smart          Use smart punctuation\n");
	printf("  --normalize      Consolidate adjacent text nodes\n");
	printf("  --verbose        go into verbose mode\n");
	printf("  --include <file> include file in <HEAD>\n");
	printf("  --help, -h       Print usage information\n");
	printf("  --version        Print version\n");
}

// if we are writing to files (and not stdout), then basename part of
// firstFname is the basename of the files we will be writing out.  If
// outdir was set, then we ignore the path component of firstFname
static void print_document(cmark_node *document, writer_format writer,
                           int options, int width, const char* firstFname)
{
	char *result;

	initQuestionCounter();

	switch (writer) {
	case FORMAT_HTML:
		result = cmark_render_html(document, options);
		break;
	case FORMAT_XML:
		result = cmark_render_xml(document, options);
		break;
	case FORMAT_MAN:
		result = cmark_render_man(document, options);
		break;
	case FORMAT_COMMONMARK:
		result = cmark_render_commonmark(document, options, width);
		break;
	default:
		fprintf(stderr, "Unknown format %d\n", writer);
		exit(1);
	}
	if (options & CMARK_OPT_USEFILES) {
	  char* buffer = calloc(10+strlen(firstFname)+((outputDir!=NULL)?strlen(outputDir):1), 1);
	  char* q = buffer;

	  const char* p = firstFname;
	  if (outputDir!=NULL) {
	    p = strrchr(firstFname, '/');
	    if (p != NULL) p++; else p = firstFname;
	    strcpy(buffer, outputDir);
	    strcat(buffer, "/");
	    q = buffer + strlen(buffer);
	  }
	  char* ext = strrchr(p, '.');
	  if (ext == NULL) {
	    fprintf(stderr, "Expected an input file with an extension, [%s]", firstFname);
	    exit(-1);
	  }
	  while (p != ext) {
	    *q++ = *p++;
	  }
	  strcpy(q, ".html");
	  printf("Output will be in %s\n", buffer);
	  FILE* out = fopen(buffer, "w");
	  if (out == NULL) {
	    fprintf(stderr, "Could not open [%s] for writing\n", buffer);
	    exit(-1);
	  }
	  fprintf(out, "%s", result);
	  fclose(out);
	  if (options & CMARK_OPT_EXAM) {
	    // now output exam info
	    strcpy(q, ".exam");
	    printf("Exam Info output will be in %s\n", buffer);
	    FILE* out = fopen(buffer, "w");
	    if (out == NULL) {
	      fprintf(stderr, "Could not open [%s] for writing\n", buffer);
	      exit(-1);
	    }
	    if (cmark_write_exam_output(out)) {
	      // we got some kind of error
	      fclose(out);
	      if (deleteOnWarning) {
		unlink(buffer);
		strcpy(q, ".html");
		unlink(buffer);
	      }
	      exit(-1);
	    }
	    fclose(out);
	  }
	} else {
	  printf("%s", result);
	}
	free(result);
}

static void
checkDOS(char* buffer, size_t bytes, char* from)
{
  int cntr = 0;
  for (unsigned int i=0; i<bytes; i++) {
    if (buffer[i] == '\r') {
      cntr++;
    }
  }
  if (cntr > 0) {
    fprintf(stderr, "%s:file %s has %d \\r's in it.\n", 
	    nodos ? "Error": "Warning", from, cntr);
    if (nodos) exit(-1);
  }
}


int main(int argc, char *argv[])
{
  int i =1;
  int numfps = 0;
  int numincludes = 0;
  int *files, *includes;
  char buffer[4096];
  cmark_parser *parser;
  size_t bytes;
  cmark_node *document;
  int width = 0;
  char *unparsed;
  writer_format writer = FORMAT_HTML;
  int options = CMARK_OPT_DEFAULT;

#if defined(_WIN32) && !defined(__CYGWIN__)
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  files = (int *)malloc(argc * sizeof(*files));
  includes = (int *)malloc(argc *sizeof(*includes));
  while(i<argc) {
    if (strcmp(argv[i], "--version") == 0) {
      printf("cmark %s", CMARK_VERSION_STRING);
      printf(" - CommonMark converter\n(C) 2014, 2015 John MacFarlane\n");
      exit(0);
    } else if (strcmp(argv[i], "--sourcepos") == 0) {
      options |= CMARK_OPT_SOURCEPOS;
    } else if (strcmp(argv[i], "--hardbreaks") == 0) {
      options |= CMARK_OPT_HARDBREAKS;
    } else if (strcmp(argv[i], "--smart") == 0) {
      options |= CMARK_OPT_SMART;
    } else if (strcmp(argv[i], "--normalize") == 0) {
      options |= CMARK_OPT_NORMALIZE;
    } else if (strcmp(argv[i], "--outdir") == 0) {
      outputDir = argv[i+1];
      i++;
    } else if (strcmp(argv[i], "--notstdout") == 0) {
      options |= CMARK_OPT_USEFILES;
    } else if (strcmp(argv[i], "--verbose") == 0) {
      cmark_verbose = 1;
    } else if (strcmp(argv[i], "--dosok") == 0) {
      nodos = 0;
    } else if (strcmp(argv[i], "--del") == 0) {
      deleteOnWarning = 1;
    } else if ((strcmp(argv[i], "--help") == 0) ||
	       (strcmp(argv[i], "-h") == 0)) {
      print_usage();
      exit(0);
    } else if (strcmp(argv[i], "--width") == 0) {
      i += 1;
      if (i < argc) {
	width = (int)strtol(argv[i], &unparsed, 10);
	if (unparsed && strlen(unparsed) > 0) {
	  fprintf(stderr,
		  "failed parsing width '%s' at '%s'\n",
		  argv[i], unparsed);
	  exit(1);
	}
      } else {
	fprintf(stderr,
		"--width requires an argument\n");
	exit(1);
      }
    } else if((strcmp(argv[i],"-I")==0) || (strcmp(argv[i],"--include")==0))
      {
	i++;
	int start = i;
	if(i<argc)
	  {
	    while(i<argc)
	      {
		if(can_include(argv[i]))
		  {
		    includes[numincludes++] = i;
		  }
		else
		  {
		    if(i==start)
		      {
			fprintf(stderr,"--includes requires at least one file \n");
			exit(1);
		      }

		    i-=1;
		    break;
		  }
		i++;
	      }
	  }
	else
	  {
	    fprintf(stderr,"--includes requires atleast one file \n");
	    exit(1);
	  }
      }
    else if ((strcmp(argv[i], "-t") == 0) ||
	     (strcmp(argv[i], "--to") == 0)) {
      i += 1;
      if (i < argc) {
	if (strcmp(argv[i], "man") == 0) {
	  writer = FORMAT_MAN;
	} else if (strcmp(argv[i], "html") == 0) {
	  writer = FORMAT_HTML;
	} else if (strcmp(argv[i], "xml") == 0) {
	  writer = FORMAT_XML;
	} else if (strcmp(argv[i], "commonmark") == 0) {
	  writer = FORMAT_COMMONMARK;
	} else if (strcmp(argv[i], "exam") == 0) {
	  writer = FORMAT_HTML;
	  options |= CMARK_OPT_EXAM;
	} else {
	  fprintf(stderr,"Unknown format %s\n", argv[i]);
	  exit(1);
	}
      } else {
	fprintf(stderr, "No argument provided for %s\n",
		argv[i - 1]);
	exit(1);
      }
    } else if (*argv[i] == '-') {
      print_usage();
      exit(1);
    } else { // treat as file argument
      files[numfps++] = i;
    }
    i++;
  }
    
  parser = cmark_parser_new(options);
  for (i = 0; i < numfps; i++) {
    FILE *fp = fopen(argv[files[i]], "r");
    if (fp == NULL) {
      fprintf(stderr, "Error opening file %s: %s\n", argv[files[i]], strerror(errno));
      exit(1);
    }

    start_timer();
    while ((bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
      checkDOS(buffer, bytes, argv[files[i]]);
      cmark_parser_feed(parser, buffer, bytes);
      if (bytes < sizeof(buffer)) {
	break;
      }
    }
    end_timer("processing lines");

    fclose(fp);
  }

  if (numfps == 0) {
    if (options & CMARK_OPT_USEFILES) {
      fprintf(stderr, "--stdout specified, but no input filename specified");
      print_usage();
      exit(-1);
    }
    while ((bytes = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
      checkDOS(buffer, bytes, "stdin");
      //in case user types markdown file in command line
      cmark_parser_feed(parser, buffer, bytes);
      if (bytes < sizeof(buffer)) {
	break;
      }
    }
  }

  start_timer();
  document = cmark_parser_finish(parser);
  cmark_parser_free(parser);
  //In case additional files were passed in to be included in the command line
  cmark_include_files(document,argv,includes,numincludes);
  end_timer("finishing document");

  start_timer();
  print_document(document, writer, options, width, argv[files[0]]);
  end_timer("print_document");

  start_timer();
  cmark_node_free(document);
  end_timer("free_blocks");

  free(files);
  free(includes);

  return 0;
}
