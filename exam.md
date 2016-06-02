Nodes which are used to create exam questions with their associated form entries.

{toc}

# Issues

- if a `{toc}` or `{question}` appear with text on same line, then we get it as plain text in a paragraph node
- if we have a `{toc}` and no entries, we generate it anyway.

# The question

`{question:level}` would output a numbered question at `level`.  If
`level` is not specified, default to the top level, e.g., 1.  The
expectation is that we should get some kind of answer set folling
this.

# Multiple choice

`{radio:begin}`  stuff  `{radio:end}`  will mark off the choices.  Each
choice will be on a seperate line

`{answer}text`

The `{answer}` can take optional stuff which is not output, such as
`{answer:this is the right answer}`

# choose from a set of options

`{check:begin}` stuff `{check:end}` will mark off the choices.  Choices as
in multiple choice

begin and end tokens must start on a new line.

# Fill in the blank

`{blank}`  or `{blank:width}`  The first produces a textbox of length 5,
otherwise width spaces

# Fill in an area

`{blank:area:height}`

must start on a new line and will produce a text box the width of the
screen and height lines long

# Matching question

`{match:left:begin}` left hand column choices `{match:left:end}`
`{match:right:begin}` right hand column choices `{match:right:end}`

begin tokens must start on a new line.

The choices are same syntax as multiple choice.  Will produce a text
box next to each left hand column entry where you can enter the number
of the item you want on the right.

# Tables

Use the following spec:
<https://github.com/adam-p/markdown-here/wiki/Markdown-Cheatsheet#tables>

# Equations

Also, I will probably need some way to produce equations.  (Probably
by exporting the code and generating an image)
