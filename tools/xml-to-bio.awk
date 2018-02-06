function handle_tag(w)
{
  if(substr(w,2,1) == "/") {
    tag = substr(w,3,length(w)-3)
    if(tag_top == 0) {
      print "Closing tag on empty stack";
      exit;
    }
    if(tag_stack[tag_top-1] != tag) {
      print "Tag mismatch",tag_stack[tag_top-1],tag;
      exit;
    }
    tag_top--;
    if(tag_beg > tag_top)
      tag_beg = tag_top;

  } else {
    tag = substr(w,2,length(w)-2)
    tag_stack[tag_top++] = tag;
  }
}

function handle_word(w)
{
  if(tag_top != 0) {
    printf("%s", w)
    for(j=0;j<tag_beg;j++)
      printf("%ci-%s", j == 0 ? " " : "_", tag_stack[j]);
    for(j=tag_beg;j<tag_top;j++)
      printf("%cb-%s", j == 0 ? " " : "_", tag_stack[j]);
    printf("\n")
  } else
    print w,"o";
  nl=0

  tag_beg = tag_top;
  if(w == ".") {
    print "";
    nl=1
  }
}

function handle(w)
{
  if(w != "<<" && substr(w,1,1) == "<") {
    handle_tag(w);
  } else
    handle_word(w);
}

BEGIN {
  tag_top = 0;
  tag_beg = 0;
}

{
  for(i=1;i<=NF;i++)
    handle($i);
  print("\n")
}

END {
  if(nl != 1)
    print ""
}
