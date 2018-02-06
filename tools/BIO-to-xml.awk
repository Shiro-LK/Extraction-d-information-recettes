BEGIN {
  first = 1;
  ct="";
}

{
  if(NF == 0) {
    if(ct != "") {
      printf(" </%s>", ct);
      ct = "";
    }
    first=1;
    printf("\n");
  } else {
    if(!first)
      printf(" ");
    else
      first=0;
    if($NF=="O") {
      if(ct != "") {
	printf("</%s> ", ct);
	ct = "";
      }
    } else {
      t = substr($NF, 3);
      f = substr($NF, 1, 1);
      if(ct != t || f == "B") {
	if(ct != "")
	  printf("</%s> ", ct);
	printf("<%s> ", t);
	ct = t;
      }
    }
    printf("%s", $1);
  }
}

END {
  if(!first) {
    if(ct != "")
      printf(" </%s>", ct);
    printf("\n");
  }
}
