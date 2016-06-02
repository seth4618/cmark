### Some helpful notes for further extending the standard markdown parser cmark


This document contains some notes on stuff that I thought was helpful to know when I added features to the existing markdown parser cmark

The following is a summary of changes I implemented:

- Added inline links with the syntax {#link} to create a link and it
  can be referenced with either <#link> or [Link] (#link)

- Added ability to include external files add add it to head/body
  using the << syntax. Currently supports css and js files. Depending
  on the method of inclusion, the whole file will be pasted, or a link
  to the stylesheet will be added in a newly created head node. (The
  rest of the file will be in a body node). Using the same syntax we
  can paste a new markdown file within an existing one.  Also added
  command line interface

- Added table of contents with the syntax {toc}. Optionally you can
  specify what depth to go to using {toc:depth} where depth is a
  number between 1 and 6. This will automatically indent your headers
  and add links with the page. So a h2 within a h1 will appear like

<ol>
<li> h1
<ol> 
<li> h2 </li></ol>
</li>
</ol>

Look at tests11-16.md/outputs11-16.html in the new_tests file for more information.


### Notes on what each file contains

A look at the src directory quickly tells that there are a lot of
files. Fortunately we don't have to modify all of them. I would
recommend looking at the ```cmark.h``` file first. This file contains
all the definitions for data structures, nodes and AST manipulation
functions and contains useful comments on what each function
does. This is the place where you will have to modify first if you
want to add new nodes. Depending on the kind of node whether it is
block (can contain other nodes) or inline, add the node to appropriate
node_type. After doing this there are some functions that have switch
statements that need to be modified to reflect the new node such as
```cmark_node_get_type_string``` in `node.c`. My recommendation is to also add the
appropriate code for cmark_node_free now before you forget that later.

Functions which test node->type and probably will need to be agumented
if you add a new type (there are other functions, but many of them
seem to test a single property and thus probably won't need to be
changed):

- `static bool S_can_contain(cmark_node *node, cmark_node *child)`
- `const char* cmark_node_get_type_string(cmark_node *node)`


```blocks.c``` is probably the most important file. It contains the
core of all the parsing functions. Without going into too much detail,
the input file is parsed incrementally, thereby constructing the
abstract syntax tree. To better get a feel for how the parsing worked,
I tried adding print statements inside the ```S_parser_feed```
function and experimenting with markdown files containing just one
piece of syntax like an reference style link, a header and so
on. Block elements like paragraphs, lists and headers are parsed
here. Most often, you will rarely have to add to the S_parser_feed
directly. Instead you can find an existing piece of syntax that is
pretty similar to what you're trying to implement and you can add the
code for parsing your new element here.


```inlines.c``` contains all the functions for parsing inline
elements, such as images, emphasis and so on. This is where the code
for the inline link is implemented. The main function is parse_inline
that processes groups of characters, pausing at special characters to
handle them appropriately. To detect the special characters that have
special meaning the code uses a large array, with 1's denote locations
of the special character. So if you're adding an additional special
character, make sure to change that here. For example, I had to add {
and } for the inline links

```main.c``` as expected is where the command line parsing takes
place. I added the code for parsing command line to include files
arguments inside here. ```references.c``` contains the implementation
to deal with link references like ```[Link]][Reference] [Reference]
(http://www.google.com)``` using a hash table

If you are thinking about adding a new piece of syntax chances are you
have to create a new regular expression to detect the token. These new
regular expressions are added in the scanners.re file. scanners.re
uses a package called re2c to convert a file with specified regular
expressions in some format to a file called scanners.c that contains a
bunch of switch statements to detect if a token match has been
found. The Makefile contains the flags used with re2c. I believe some
of the flags only work with some versions. The Makefile contains more
details.


```html.c``` contains the code for actually writing the html
file. Although cmark supports other formats like xml, man pages,
etc. the most popular one is html. The whole html file is maintained
in a variable called html that is a ```cmark_strbuf```. ```buffer.h```
contains all the interface functions to interface with the this
```cmark_strbuf```, but basically its a string buffer where you can
just use ```cmark_strbuf_puts``` to add some text into the file. It
behaves like an unbounded array and the functions to deal with growing
the buffer, etc. are all already in the ```buffer.h``` file.

All the way down you will notice that the ```html.c``` uses an
iterator to walk the abstract syntax tree and render nodes
accordingly. The iterator is a data structure that allows for
effective traversal of the AST. At each node, the iterator contains a
variable that tells if you are entering or exiting the node, so it
walks down the tree in an intuitive way and this helps generate the
html document because now depending on whether you are entering or
exiting all you have to do is handle the tags to open or close. Also
keep in mind that only the node of type NODE_TEXT actually displays
text. All the other nodes merely specify which tags are being used and
render pieces like ```<h1>```. The text nodes are created in the
```inlines.c``` funcion, when block nodes like paragraphs and headers
are processed.

The basic data structure of a node in a tree is in ```node.h``` and it
contains a whole bunch of fields. I would recommend looking at this
file too right after ```cmark.h```. Some important fields include a
union ```as``` which can be used to interpret the node as a header, a
link, a literal, etc. It contains a field called user_data which you
can use for holding some data for your new node if you don't feel like
adding to/modifying the parser and/or the node interface. It also has
variables determining the start line, end line, start column, end
column, etc. which can be used to determine indentation and whether
there is stuff in the surrounding lines. If you want to create a tag
that is separate from the rest of the text like the Reference style
link [Link](http://www.google.com) (which must be separated from the
rest of the text by new lines according to the official markdown
specification) you can adapt the way this is parsed to your new
node. I did this for the include files ```<<``` and table of contents
```{toc}``` syntax

### Notes specific to adapting to exam questions

As for the different questions, it looks like a lot of them are almost
small forms in themselves. By forms I mean using the ```<form>```
tag. So you could start by creating a new node for each kind of
question. I realize it would be cleaner to have a NODE_QUESTION and
maybe as a union have all the different types, but this would involve
quite a few changes to the exising node structure, so it might be more
compatible to create new nodes for each kind of question. Maybe you
could have a NODE_QUESTION and use the user_data field to specify what
kind of question it is. In addition I think you might need a
NODE_CHOICE or a NODE_SUBQUESTION to specify the options or further
details.

This initial quesion node could tell you the question itself and the
subsequent choices could tell you what kind of input the form
takes. So maybe if its a Multiple Choice question you could create a
form with input nodes of type radio. So the NODE_QUESTION could
contain the text of the actual question and you could have a
NODE_CHOICE could be a generic container for choices and maybe you
could case on what kind of container it is when you actually render
the node i.e all the choices will have the tag name as ```input``` but
the type could be ```textarea```, ```radio```, ```text```,
```checkbox```, etc. And similarly for the answer maybe you could have
a separate NODE_ANSWER that is a generic container and you could set a
field in the node struct that you could use to determine what kind of
answer it is when you display the node in the rendering phase. This
can apply to the match question too, where you could have the
NODE_QUESTION node contain question and any one of the possible
matches and then have a NODE_CHOICE contain a ```input``` tag of the
type ```text```

For the general formatting, if there are is a lot of css involved,
maybe you could use a field in the node struct to specify a class name
and when you render the node, render it with the class. For example in
the ```html.c``` file, the ```S_render_node``` function has a case for
NODE_TOC that gets rendered with a ```class="toc"``` that can allow
the user to specify the formatting for the table of contents by
writing style specifications for ```.toc``` in a css file. You can
then nclude that css file in the command line or use the ```<<```
syntax

I think this is one way you in which you could use the above general
template for any kind of question. As for tables, I believe the
original markdown implmentation (cmark) does NOT support
tables. However there are many other markdown versions that support
tables. For equations also, again the original cmark implementation
does not support this, but there are others that do this. For example:

<http://fletcher.github.io/MultiMarkdown-4/math.html>

But you could also have a LATEX Node, which you could use when there
is text like $ equation $ \[ equation ]\ that you could render just
like that/write to a new file when you encounter these nodes and then
you could convert the existing file/new file that you wrote to a pdf
using some latex to pdf software. Or you could just save the equations
as images and insert them into the markdown code normally using the
```![Link](link url)``` syntax