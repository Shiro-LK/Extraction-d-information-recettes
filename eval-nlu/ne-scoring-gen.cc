#undef _FORTIFY_SOURCE
#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 500

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <list>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <iostream>

using namespace std;

// Options
static const char *progname;
static bool opt_summary, opt_details, opt_details_correct, opt_iag, opt_ref_aref, opt_open;
static int opt_expected_count;

// Tag stuff
static vector<string> tag_names;
static map<string, int> tag_names_map;
static vector<int> tag_hypcount, tag_refcount, tag_correct;

// Error keys stuff
static vector<string> error_names;
static map<string, int> error_names_map;

struct error_d {
  double cost;
  list<int> error_types;

  error_d() { cost = -1; }
};


// A tag alone in an input file
struct simple_tag {
  int tagid;                        // Number representing the tag name
  bool closing;                     // Is it a closing tag?
  int pos;                          // Offset in bytes from the start of the post-extraction text
  int line, col;                    // Position in the original xml (for bitching purposes)
  list<pair<string, string> > attr; // Attribute/value pairs

  simple_tag(int _tagid, bool _closing, int _pos, int _line, int _col, const list<pair<string, string> > &_attr) {
    tagid = _tagid; closing = _closing; pos = _pos; line = _line; col = _col; attr = _attr;
  }
};

struct aref_tag {
  int id;
  int tagid;
  int pos;
  bool opening, closing;
  int depth;
  int parent;
  int line, col;                    // Position in the original xml (for bitching purposes)
  list<pair<string, string> > attr; // Attribute/value pairs

  aref_tag(int _id, int _tagid, int _pos, bool _opening, bool _closing, int _depth, int _parent, int _line, int _col) {
    id = _id; tagid = _tagid; pos = _pos; opening = _opening; closing = _closing; depth = _depth;
    parent = _parent; line = _line; col = _col;
  }
};

// A tagged entity
struct entity {
  int tagid;                                              // Number representing the tag name
  vector<int> start, end;                                 // Start and end positions
  list<pair<string, string> > attr;                       // Attribute/value pairs
  int line, col;                                          // Position in the source file of the starting tag
  int depth;                                              // Depth of the entity, starts at 0
  bool hyp;                                               // Hypothesis entity or reference ?
  struct entity *parent;                                  // Id of the parent entity, 0 if none
  struct entity *left_constraint;                         // Id of the entity on the left of that one with the same parent (or no parent for either), 0 if none

  vector<vector<error_d> > miss_errors;                   // Miss error depending on the frontiers chosen
  map<entity *, vector<vector<error_d> > > subst_errors;  // Substitution error depending on the frontiers chosen and the hypothesis entity - reference entities only
  bool paired;                                            // Is this entity paired with another in the mapping

  entity() { tagid = -1; }
  entity(int _tagid, int _line, int _col, int _depth, bool _hyp, const list<pair<string, string> > &_attr) {
    tagid = _tagid; depth = _depth; line = _line; col = _col; hyp = _hyp; attr = _attr; parent = 0; left_constraint = 0;
  }
};


// A segment of text between two entities frontiers
struct segment {
  struct pairinfo {
    entity *er, *eh;
    const error_d *error;

    pairinfo(entity *_er, entity *_eh, const error_d *e) { er = _er; eh = _eh; error = e; }
  };

  struct ef {
    entity *e;
    unsigned int fid;
    ef(entity *_e, unsigned int _fid) { e=_e; fid=_fid; }
  };

  int start, end;                         // position of the start and the end of the segment
  vector<entity *> entities;              // entities present in the segment
  vector<ef> starting_ref_entities;       // reference entities which may start here
  vector<ef> ending_ref_entities;         // reference entities which may stop here
  vector<entity *> starting_hyp_entities; // hypothesis entities which start here

  list<pairinfo> added_pairs;             // Pairs added within the segment
  list<entity *> unmapped_entities;       // Entities that could have been mapped within the segment (e.g. starting there) but haven't
};

// Escape a string for printing, deduplicate spaces
void escape(char *dest, const char *src, int size)
{
  unsigned char pc = 0;
  while(*src && size > 0) {
    unsigned char c = *src++;
    if(c == 10) {
      *dest++ = '\\';
      *dest++ = 'n';
    } else if(c < 32) {
      sprintf(dest, "\\0x%02x", c);
      dest += 5;
    } else if(c != 32 || c != pc)
      *dest++ = c;
    pc = c;
    size--;
  }
  *dest = 0;
}

// Load a file, tack an \0 at the end
char *file_load(const char *fname)
{
  char msg[512];

  sprintf(msg, "Open %s", fname);
  int fd = open(fname, O_RDONLY);
  if(fd<0) {
    perror(msg);
    exit(2);
  }

  int size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  char *data = (char *)malloc(size+1);
  read(fd, data, size);
  close(fd);
  data[size] = 0;
  return data;
}

// Get a id from a name, create it if needed
int any_get(string t, vector<string> &vt, map<string, int> &mt)
{
  map<string, int>::const_iterator i = mt.find(t);
  if(i != mt.end())
    return i->second;
  int id = vt.size();
  vt.push_back(t);
  mt[t] = id;
  return id;
}

// Get a tagid from a tag name, create it if needed
int tag_get(string t)
{
  return any_get(t, tag_names, tag_names_map);
}

// Get a tagid from a tag name, -1 if not a tested tag
int tag_find(string t)
{
  map<string, int>::const_iterator i = tag_names_map.find(t);
  return i != tag_names_map.end() ? i->second : -1;
}

// Get an errid from an error name, create it if needed
int error_get(string t)
{
  return any_get(t, error_names, error_names_map);
}

string lua_tocxxstring(lua_State *L, int idx)
{
  size_t sz;
  const char *str = lua_tolstring(L, idx, &sz);
  if(!str)
    return "";
  return string(str, sz);
}

void lua_pushcxxstring(lua_State *L, string s)
{
  lua_pushlstring(L, s.data(), s.size());
}

void lua_pushentity(lua_State *L, const entity *e, int sf, int ef, const char *data)
{
  lua_newtable(L);
  lua_pushcxxstring(L, tag_names[e->tagid]);
  lua_setfield(L, -2, "type");
  lua_pushboolean(L, e->hyp);
  lua_setfield(L, -2, "hyp");
  lua_pushinteger(L, e->start[sf]);
  lua_setfield(L, -2, "spos");
  lua_pushinteger(L, e->end[ef]);
  lua_setfield(L, -2, "epos");

  if(!e->attr.empty()) {
    lua_newtable(L);
    for(list<pair<string, string> >::const_iterator i = e->attr.begin(); i != e->attr.end(); i++) {
      lua_pushcxxstring(L, i->first);
      lua_pushcxxstring(L, i->second);
      lua_rawset(L, -3);
    }
    lua_setfield(L, -2, "attr");
  }

  char *ebuf = new char[5*(e->end[ef] - e->start[sf])];
  escape(ebuf, data + e->start[sf], e->end[ef] - e->start[sf]);
  lua_pushstring(L, ebuf);
  delete[] ebuf;
  lua_setfield(L, -2, "value");
}

void load_lua_description(lua_State *L, const char *fname)
{
  luaL_openlibs(L);

  if(luaL_loadfile(L, fname)) {
    fprintf(stderr, "Error loading %s: %s\n", fname, lua_tostring(L, -1));
    exit(1);
  }

  if(lua_pcall(L, 0, 0, 0)) {
    fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
    exit(1);
  }
}

/*
void lua_get_global_function(lua_State *L, const char *fname)
{
  lua_getfield(L, LUA_GLOBALSINDEX, fname);
  if(!lua_isfunction(L, -1)) {
    fprintf(stderr, "Error in lua description: function %s not found\n", fname);
    exit(1);
  }
} */

void lua_get_global_function(lua_State *L, const char *fname)
{
  lua_getglobal(L, fname);
  if(!lua_isfunction(L, -1)) {
    fprintf(stderr, "Error in lua description: function %s not found\n", fname);
    exit(1);
  }
}

void lua_do_call(lua_State *L, const char *fname, int np, int nr)
{
  if(lua_pcall(L, np, nr, 0)) {
    fprintf(stderr, "Error calling %s: %s\n", fname, lua_tostring(L, -1));
    exit(1);
  }
}

void lua_load_error(lua_State *L, error_d &error, const char *fname)
{
  error.cost = lua_tonumber(L, 1);
  if(lua_isnil(L, 2))
    return;
  if(lua_isstring(L, 2)) {
    error.error_types.push_back(error_get(lua_tostring(L, 2)));
    return;
  }
  if(lua_istable(L, 2)) {
    lua_pushvalue(L, 2);
    for(int i=1;;i++) {
      lua_rawgeti(L, 2, i);
      if(lua_isnil(L, -1))
	break;
      const char *err = lua_tostring(L, -1);
      if(!err) {
	fprintf(stderr, "Error in lua description: %s should return an array of error names and entry %d is not a string.\n", fname, i);
	exit(1);
      }
      error.error_types.push_back(error_get(err));
      lua_pop(L, 1);
    }
    lua_pop(L, 2);
    error.error_types.sort();
    return;
  }
  fprintf(stderr, "Error in lua description: %s should return as a second paramter nothing, a string or an array of strings.", fname);
  exit(1);
}

void load_tag_list(lua_State *L)
{
  lua_get_global_function(L, "get_all_tags");
  lua_do_call(L, "get_all_tags", 0, 1);
  if(!lua_istable(L, 1)) {
    fprintf(stderr, "Error in lua description: get_all_tags should return an array of type names.\n");
    exit(1);
  }
  for(int i=1;;i++) {
    lua_rawgeti(L, 1, i);
    if(lua_isnil(L, -1))
      break;
    if(!lua_isstring(L, -1)) {
      fprintf(stderr, "Error in lua description: get_all_tags should return an array of type names and entry %d is not a string.\n", i);
      exit(1);
    }
    tag_get(lua_tocxxstring(L, -1));
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
}



// Extract relevant tags with their positions, leave the other ones in

#define step_test() do { if(*p == '\n') { line++; col = 0; } else col++; } while(0)
#define advance_on(expr) do { while(expr) { step_test(); p++; } } while(0)

void xml_extract_tags(list<simple_tag> &tags, char *data, const char *fname)
{
  const char *p = data;
  char *q = data;
  int line = 1, col = 0;
  while(*p) {
    while(*p && (*p != '<' || ((p[1] < 'a' || p[1] > 'z') && p[1] != '/'))) {
      step_test();
      *q++ = *p++;
    }

    if(!*p)
      break;
    const char *sp = p;
    int sline = line, scol = col;

    p++;
    col++;
    advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

    bool closing = false;
    if(*p == '/') {
      closing = true;
      p++;
      col++;
      advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');
    }

    const char *tstart = p;
    advance_on(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>');
    string tag(tstart, p);

    int tid = tag_find(tag);
    if(tid != -1) {
      list<pair<string, string> > attr;

      while(*p && *p != '>') {
	advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');
	tstart = p;
	advance_on(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>' && *p != '=');
	string attr_type(tstart, p);

	advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

	if(!*p || *p == '>') {
	  if(!attr_type.empty())
	    attr.push_back(pair<string, string>(attr_type, ""));
	  break;
	}
	      
	if(attr_type.empty()) {
	  fprintf(stderr, "%s:%d:%d: Error: Malformed tag, stray '='.\n", fname, line, col);
	  exit(1);
	}

	if(*p != '=') {
	  attr.push_back(pair<string, string>(attr_type, ""));
	  continue;
	}

	p++;
	col++;
	advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

	if(!*p || *p == '>')
	  break;

	string value;
	if(*p == '"') {
	  p++;
	  col++;
	  tstart = p;	  
	  advance_on(*p && *p != '"');
	  if(*p != '"') {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed tag, missing closing quote.\n", fname, line, col);
	    exit(1);
	  }
	  value = string(tstart, p);
	  p++;
	  col++;

	} else {
	  tstart = p;
	  advance_on(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>');
	  value = string(tstart, p);
	}
	attr.push_back(pair<string, string>(attr_type, value));
      }

      if(!*p) {
	fprintf(stderr, "%s:%d:%d: Error: Malformed tag, missing '>'.\n", fname, line, col);
	exit(1);
      }

      p++;
      col++;

      tags.push_back(simple_tag(tid, closing, q-data, sline, scol, attr));

    } else {
      memmove(q, sp, p-sp);
      q += p-sp;
    }
  }
  *q = 0;
}

/*
void aref_extract_tags(list<aref_tag> &tags, char *data, const char *fname)
{
  const char *p = data;
  char *q = data;
  int line = 1, col = 0;
  while(*p) {
    while(*p && *p != '<') {
      step_test();
      *q++ = *p++;
    }

    if(!*p)
      break;
    const char *sp = p;
    int sline = line, scol = col;

    p++;
    col++;
    advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

    bool closing = false;
    if(*p == '/') {
      closing = true;
      p++;
      col++;
      advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');
    }

    const char *tstart = p;
    advance_on(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>');
    string annot(tstart, p);
    if(annot == "annotation") {
      bool has_id = false;
      bool has_tagid = false;
      bool has_frontiers = false;
      bool has_depth = false;
      bool has_parent = false;

      int val_id = 0;
      int val_tagid = 0;
      bool val_opening = false, val_closing = false;
      int val_depth = 0;
      int val_parent = -1;


      list<pair<string, string> > attr;

      while(*p && (*p != '/' || p[1] != '>')) {
	advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');
	tstart = p;
	advance_on(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>' && *p != '=' && *p != '/');
	string type(tstart, p);

	advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

	if(!*p || (*p == '/' && p[1] == '>'))
	  break;
	      
	if(type.empty()) {
	  fprintf(stderr, "%s:%d:%d: Error: Malformed tag, stray '='.\n", fname, sline, scol);
	  exit(1);
	}

	string value;
	if(*p == '=') {
	  p++;
	  col++;
	  advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

	  if(!*p || (*p == '/' && p[1] == '>'))
	    break;

	  if(*p == '"') {
	    p++;
	    col++;
	    tstart = p;	  
	    advance_on(*p && *p != '"');
	    if(*p != '"') {
	      fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, missing closing quote.\n", fname, sline, scol);
	      exit(1);
	    }
	    value = string(tstart, p);
	    p++;
	    col++;
	    
	  } else {
	    tstart = p;
	    advance_on(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>' && *p != '/');
	    value = string(tstart, p);
	  }
	}

	if(type == "id") {
	  if(has_id) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate id.\n", fname, sline, scol);
	    exit(1);
	  }
	  has_id = true;
	  val_id = strtol(value.c_str(), 0, 10);

	} else if(type == "type") {
	  if(has_tagid) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate type.\n", fname, sline, scol);
	    exit(1);
	  }
	  has_tagid = true;
	  val_tagid = tag_find(value);
	  if(val_tagid == -1) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, unknown type %s.\n", fname, sline, scol, value.c_str());
	    exit(1);
	  }

	} else if(type == "ftype") {
	  if(has_frontiers) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate ftype.\n", fname, sline, scol);
	    exit(1);
	  }
	  if(value != "s" && value != "e" && value != "se") {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, unknown ftype %s.\n", fname, sline, scol, value.c_str());
	    exit(1);
	  }

	  val_opening = value == "s" || value == "se";
	  val_closing = value == "e" || value == "se";

	} else if(type == "depth") {
	  if(has_depth) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate depth.\n", fname, sline, scol);
	    exit(1);
	  }

	  val_depth = strtol(value.c_str(), 0, 10);

	} else if(type == "parent") {
	  if(has_parent) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate parent.\n", fname, sline, scol);
	    exit(1);
	  }

	  val_parent = strtol(value.c_str(), 0, 10);

	} else {
	  fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, unknown attribute type %s.\n", fname, sline, scol, value.c_str());
	  exit(1);
	}
      }

      if(!*p) {
	fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, missing '/>'.\n", fname, sline, scol);
	exit(1);
      }

      p+=2;
      col++;

      tags.push_back(aref_tag(val_id, val_tagid, q-data, val_opening, val_closing, val_depth, val_parent, sline, scol));

    } else {
      memmove(q, sp, p-sp);
      q += p-sp;
    }
  }
  *q = 0;
}
*/

void aref_extract_tags(list<aref_tag> &tags, char *data, const char *fname)
{
  const char *p = data;
  char *q = data;
  int line = 1, col = 0;
  while(*p) {
    while(*p && *p != '<') {
      step_test();
      *q++ = *p++;
    }

    if(!*p)
      break;
    const char *sp = p;
    int sline = line, scol = col;

    p++;
    col++;
    advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

    if(*p == '/') {
      p++;
      col++;
      advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');
    }

    const char *tstart = p;
    advance_on(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>');
    string annot(tstart, p);
    if(annot == "annotation") {
      bool has_id = false;
      bool has_tagid = false;
      bool has_frontiers = false;
      bool has_depth = false;
      bool has_parent = false;

      int val_id = 0;
      int val_tagid = 0;
      bool val_opening = false, val_closing = false;
      int val_depth = 0;
      int val_parent = -1;

      list<pair<string, string> > attr;

      while(*p && (*p != '/' || p[1] != '>')) {
	advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');
	tstart = p;
	advance_on(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>' && *p != '=' && *p != '/');
	string type(tstart, p);

	advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

	if(!*p || (*p == '/' && p[1] == '>'))
	  break;
	      
	if(type.empty()) {
	  fprintf(stderr, "%s:%d:%d: Error: Malformed tag, stray '='.\n", fname, sline, scol);
	  exit(1);
	}

	string value;
	if(*p == '=') {
	  p++;
	  col++;
	  advance_on(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');

	  if(!*p || (*p == '/' && p[1] == '>'))
	    break;

	  if(*p == '"') {
	    p++;
	    col++;
	    tstart = p;	  
	    advance_on(*p && *p != '"');
	    if(*p != '"') {
	      fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, missing closing quote.\n", fname, sline, scol);
	      exit(1);
	    }
	    value = string(tstart, p);
	    p++;
	    col++;
	    
	  } else {
	    tstart = p;
	    advance_on(*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>' && *p != '/');
	    value = string(tstart, p);
	  }
	}

	if(type == "id") {
	  if(has_id) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate id.\n", fname, sline, scol);
	    exit(1);
	  }
	  has_id = true;
	  val_id = strtol(value.c_str(), 0, 10);

	} else if(type == "type") {
	  if(has_tagid) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate type.\n", fname, sline, scol);
	    exit(1);
	  }
	  has_tagid = true;
	  val_tagid = tag_find(value);
	  if(val_tagid == -1) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, unknown type %s.\n", fname, sline, scol, value.c_str());
	    exit(1);
	  }

	} else if(type == "ftype") {
	  if(has_frontiers) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate ftype.\n", fname, sline, scol);
	    exit(1);
	  }
	  if(value != "s" && value != "e" && value != "se") {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, unknown ftype %s.\n", fname, sline, scol, value.c_str());
	    exit(1);
	  }

	  val_opening = value == "s" || value == "se";
	  val_closing = value == "e" || value == "se";

	} else if(type == "depth") {
	  if(has_depth) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate depth.\n", fname, sline, scol);
	    exit(1);
	  }

	  val_depth = strtol(value.c_str(), 0, 10);

	} else if(type == "parent") {
	  if(has_parent) {
	    fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, duplicate parent.\n", fname, sline, scol);
	    exit(1);
	  }

	  val_parent = strtol(value.c_str(), 0, 10);

	} else {
	  fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, unknown attribute type %s.\n", fname, sline, scol, value.c_str());
	  exit(1);
	}
      }

      if(!*p) {
	fprintf(stderr, "%s:%d:%d: Error: Malformed annotation, missing '/>'.\n", fname, sline, scol);
	exit(1);
      }

      p+=2;
      col++;

      tags.push_back(aref_tag(val_id, val_tagid, q-data, val_opening, val_closing, val_depth, val_parent, sline, scol));

    } else {
      memmove(q, sp, p-sp);
      q += p-sp;
    }
  }
  *q = 0;
}

#undef step_test
#undef advance_on

// Load an annotated file and extract its tags
void annotated_file_load(const char *name, list<simple_tag> &tags, char *&data)
{
  data = file_load(name);
  xml_extract_tags(tags, data, name);
}

// Load an annotated file and extract its tags
void aref_file_load(const char *name, list<aref_tag> &tags, char *&data)
{
  data = file_load(name);
  aref_extract_tags(tags, data, name);
}

// Align pos-extraction reference and hypothesis to sync the hypothesis tag positions
void align_and_reposition(const char *ref_data, const char *hyp_data, list<simple_tag> &hyp_tags)
{
  const char *rp = ref_data, *hp = hyp_data;
  list<simple_tag>::iterator i = hyp_tags.begin();
  int ref_line = 1, hyp_line = 1;
  for(;;) {
    const char *hd = i != hyp_tags.end() ? hyp_data + i->pos : 0;
    while((*rp || *hp) && hp != hd) {
      char rc = *rp;
      if(rc == ' ' || rc == '\t' || rc == '\r' || rc == '\n') {
	if(rc == '\n')
	  ref_line++;
	rp++;
	continue;
      }

      char hc = *hp;
      if(hc == ' ' || hc == '\t' || hc == '\r' || hc == '\n') {
	if(hc == '\n')
	  hyp_line++;
	hp++;
	continue;
      }

      if(rc != hc) {
	char buf[64*5+1];
	fprintf(stderr, "Mismatch when aligning ref and hyp, hyp line %d, ref line %d:\n", ref_line, hyp_line);
	escape(buf, rp < ref_data-8 ? ref_data : rp-8, 64);
	fprintf(stderr, "  ref:  [%s]\n", buf);
	escape(buf, hp < hyp_data-8 ? hyp_data : hp-8, 64);
	fprintf(stderr, "  hyp:  [%s]\n", buf);
	exit(1);
      }
      rp++;
      hp++;
    }

    // Exit when all tags have been repositioned *and* the end of oth files is reached
    if(!hd)
      break;

    // Reaching the end of both files but not one of the extracted tags is in the "can't happen" category
    assert(hp == hd);
    i->pos = rp - ref_data;
    i++;
  }
}


// Build entities from tags
void build_entities_from_tags(vector<entity> &entities, const list<simple_tag> &tags, const char *fname, bool hyp)
{
  list<int> stack;
  for(list<simple_tag>::const_iterator i = tags.begin(); i != tags.end(); i++) {
    if(i->closing) {
      if(stack.empty()) {
	fprintf(stderr, "%s:%d:%d: Found closing %s tag without a matching opening one.\n",
		fname, i->line, i->col, tag_names[i->tagid].c_str());
	exit(1);
      }
      if(entities[stack.back()].tagid != i->tagid) {
	fprintf(stderr, "%s:%d:%d: Found closing %s tag for an opening %s tag.\n",
		fname, i->line, i->col, tag_names[i->tagid].c_str(), tag_names[entities[stack.back()].tagid].c_str());
	exit(1);
      }
      entities[stack.back()].end.push_back(i->pos);
      stack.pop_back();
    } else {
      int eid = entities.size();
      entities.push_back(entity(i->tagid, i->line, i->col, stack.size(), hyp, i->attr));
      entities[eid].start.push_back(i->pos);
      stack.push_back(eid);      
    }
  }
  if(!stack.empty()) {
    fprintf(stderr, "%s: Missing closing tag for %s (line %d) at end of file.\n",
	    fname, tag_names[entities[stack.back()].tagid].c_str(), entities[stack.back()].line);
    exit(1);    
  }
}

void build_entities_from_tags(vector<entity> &entities, const list<aref_tag> &tags, const char *fname, bool hyp)
{
  int max_id = 0;
  for(list<aref_tag>::const_iterator i = tags.begin(); i != tags.end(); i++)
    if(i->id > max_id)
      max_id = i->id;

  entities.resize(max_id+1);

  vector<entity *> entity_per_depth;
  for(list<aref_tag>::const_iterator i = tags.begin(); i != tags.end(); i++) {
    int eid = i->id;
    if(entities[eid].tagid == -1) {
      entities[eid].tagid = i->tagid;
      entities[eid].line = i->line;
      entities[eid].col = i->col;
      entities[eid].depth = i->depth;
      entities[eid].parent = i->parent == -1 ? 0 : &entities[i->parent];
      entities[eid].attr = i->attr;
      entities[eid].hyp = hyp;
      if(int(entity_per_depth.size()) > i->depth && entity_per_depth[i->depth]->parent == entities[eid].parent)
	entities[eid].left_constraint = entity_per_depth[i->depth];
      else
	entities[eid].left_constraint = 0;
      if(int(entity_per_depth.size()) <= i->depth)
	entity_per_depth.resize(i->depth+1);
      entity_per_depth[i->depth] = &entities[eid];
    }
    if(i->opening)
      entities[eid].start.push_back(i->pos);
    if(i->closing)
      entities[eid].end.push_back(i->pos);
  }
}

// Tighten entity frontiers so that they do not include whitespace
void refine_entities(vector<entity> &entities, const char *data, const char *fname)
{
  for(unsigned int i = 0; i != entities.size(); i++) {
    for(vector<int>::iterator j = entities[i].start.begin(); j != entities[i].start.end(); j++) {
      int s = *j;

      while(data[s]) {
	char c = data[s];
	if(c != ' ' && c != '\t' && c != '\r' && c != '\n')
	  break;
	s++;
      }
      *j = s;
    }

    for(vector<int>::iterator j = entities[i].end.begin(); j != entities[i].end.end(); j++) {
      int e = *j;

      while(e>0) {
	char c = data[e-1];
	if(c != ' ' && c != '\t' && c != '\r' && c != '\n')
	  break;
	e--;
      }
      *j = e;
    }

#if 0
    fprintf(stderr, "%s:%d:%d: tag %s %d start=(",
	    fname, entities[i].line, entities[i].col, tag_names[entities[i].tagid].c_str(), i);
    for(vector<int>::iterator j = entities[i].start.begin(); j != entities[i].start.end(); j++)
      fprintf(stderr, " %d", *j);
    fprintf(stderr, " ) end=(");
    for(vector<int>::iterator j = entities[i].end.begin(); j != entities[i].end.end(); j++)
      fprintf(stderr, " %d", *j);
    fprintf(stderr, " )\n");
#endif

    for(int s = entities[i].start.front(); !entities[i].end.empty() && entities[i].end.front() <= s; entities[i].end.erase(entities[i].end.begin()));
    if(entities[i].end.empty()) {
      fprintf(stderr, "%s:%d:%d: Empty tag %s.\n",
	      fname, entities[i].line, entities[i].col, tag_names[entities[i].tagid].c_str());
      exit(1);
    }

    for(int e = entities[i].end.back(); !entities[i].start.empty() && entities[i].start.back() >= e; entities[i].start.erase(entities[i].start.end()-1));
  }
}

void compute_entities_miss_costs(lua_State *L, vector<entity> &entities, const char *data)
{
  for(unsigned int i = 0; i != entities.size(); i++) {
    entities[i].miss_errors.resize(entities[i].start.size());
    for(unsigned int j=0; j != entities[i].start.size(); j++) {
      entities[i].miss_errors[j].resize(entities[i].end.size());
      for(unsigned int k=0; k != entities[i].end.size(); k++) {
	if(entities[i].start[j] < entities[i].end[k]) {
	  lua_get_global_function(L, "get_miss_cost");
	  lua_pushentity(L, &entities[i], j, k, data);
	  lua_do_call(L, "get_miss_cost", 1, 2);
	  lua_load_error(L, entities[i].miss_errors[j][k], "get_miss_cost");
	  lua_pop(L, 2);
	}
      }
    }
  }
}

void add_frontiers(map<int, list<entity *> > &frontiers, vector<entity> &entities)
{
  for(unsigned int i = 0; i != entities.size(); i++) {
    entity *e = &entities[i];
    for(vector<int>::const_iterator j = e->start.begin(); j != e->start.end(); j++)
      frontiers[*j].push_back(e);
    for(vector<int>::const_iterator j = e->end.begin(); j != e->end.end(); j++)
      frontiers[*j].push_back(e);
  }
}

void build_segments(vector<segment> &segments, const map<int, list<entity *> > &frontiers)
{
  if(!frontiers.size())
    return;

  segments.resize(frontiers.size()-1);
  set<entity *> current_entities;
  int sid = 0;
  map<int, list<entity *> >::const_iterator i = frontiers.begin();
  for(;;) {
    const list<entity *> &le = i->second;
    int start = i->first;
    i++;
    if(i == frontiers.end())
      break;
    int end = i->first;

    segment &s = segments[sid];
    s.start = start;
    s.end = end;

    for(list<entity *>::const_iterator j = le.begin(); j != le.end(); j++) {
      entity *e = *j;
      current_entities.insert(e);
      if(e->hyp) {
	if(e->start.front() == start)
	  s.starting_hyp_entities.push_back(e);
      } else {
	for(unsigned int k=0; k != e->start.size(); k++)
	  if(e->start[k] == start)
	    s.starting_ref_entities.push_back(segment::ef(e, k));

	for(unsigned int k=0; k != e->end.size(); k++)
	  if(e->end[k] == end)
	    s.ending_ref_entities.push_back(segment::ef(e, k));
      }
    }

    for(set<entity *>::iterator j = current_entities.begin(); j != current_entities.end();) {
      s.entities.push_back(*j);
      if((*j)->end.back() <= end) {
	set<entity *>::iterator k = j;
	j++;
	current_entities.erase(k);
      } else
	j++;
    }

    sid++;    
  }
}

void compute_substitution_errors_costs(lua_State *L, vector<segment> &segments, const char *data)
{
  for(vector<segment>::iterator i = segments.begin(); i != segments.end(); i++)
    for(vector<entity *>::iterator j = i->entities.begin(); j != i->entities.end(); j++) {
      entity *eh = *j;
      if(!eh->hyp)
	continue;
      for(vector<entity *>::iterator k = i->entities.begin(); k != i->entities.end(); k++) {
	entity *er = *k;
	if(er->hyp)
	  continue;
	if(er->subst_errors.find(eh) == er->subst_errors.end()) {
	  er->subst_errors[eh].resize(er->start.size());
	  int efc = er->end.size();
	  vector<vector<error_d> > &evec = er->subst_errors[eh];
	  for(unsigned int sf=0; sf != er->start.size(); sf++) {
	    evec[sf].resize(efc);
	    if(er->start[sf] >= eh->end[0])
	      continue;
	    for(unsigned int ef=0; ef != er->end.size(); ef++) {
	      if(er->end[ef] < eh->start[0])
		continue;

	      if(er->start[sf] >= er->end[ef])
		continue;

	      lua_get_global_function(L, "get_substitution_cost");
	      lua_pushentity(L, er, sf, ef, data);
	      lua_pushentity(L, eh, 0, 0, data);
	      lua_do_call(L, "get_substitution_cost", 2, 2);
	      lua_load_error(L, evec[sf][ef], "get_substitution_cost");
	      lua_pop(L, 2);
	    }
	  }
	}
      }
    }
}

struct frontier_choice {
  int sf, ef;
  frontier_choice() { sf=ef=-1; }
  frontier_choice(int _sf, int _ef) { sf = _sf; ef = _ef; }
};

static inline bool operator!=(const frontier_choice &f1, const frontier_choice &f2)
{
  return f1.sf != f2.sf || f1.ef != f2.ef;
}

int act_nodes = 0;
struct align_node {
  int refcount;
  align_node *prev;
  const segment *seg;

  double score;                                  // Score of this node (the lower the better)

  list<segment::pairinfo> added_pairs;           // Pairs added within the segment
  list<entity *> unmapped_entities;              // Entities that could have been mapped within the segment (e.g. starting there) but haven't

  list<pair<entity *, entity *> > current_pairs; // Pairs active when exiting the segment
  set<entity *> active_set;                      // Entities mapped to something and still present when exiting the segment

  map<entity *, frontier_choice> frontiers;      // Chosen frontiers

  align_node() { refcount = 1; prev = 0; score = 0; act_nodes++; seg = 0; }
  align_node(const segment *_seg, align_node *_prev) { refcount = 1; seg = _seg; prev = _prev; prev->copy_frontiers_filtered(frontiers, seg); prev->ref(); score = prev->score; act_nodes++; }
  ~align_node() {
    if(prev) {
      align_node *n = prev;
      while(n && n->refcount == 1) {
	align_node *n1 = n->prev;
	n->prev = 0;
	delete n;
	n = n1;
      }
      if(n)
	n->unref();
    }
    act_nodes--;
  }

  void ref() { refcount++; }
  void unref() { refcount--; if(!refcount) delete this; }

  bool active(entity *e) const { return active_set.find(e) != active_set.end(); }

  const frontier_choice *find_frontier(entity *e) const {
    assert(!seg || e->end.back() >= seg->start);
    map<entity *, frontier_choice>::const_iterator i = frontiers.find(e);
    if(i != frontiers.end())
      return &i->second;
    return NULL;
  }

  void add_frontier(entity *e, const frontier_choice &f) {
    assert(e->end.back() >= seg->start);
    frontiers[e] = f;
  }

  void copy_frontiers_unfiltered(map<entity *, frontier_choice> &dest) {
    for(map<entity *, frontier_choice>::const_iterator i = frontiers.begin(); i != frontiers.end(); i++)
      dest[i->first] = i->second;
  }

  void copy_frontiers_filtered(map<entity *, frontier_choice> &dest, const segment *fseg) {
    for(map<entity *, frontier_choice>::const_iterator i = frontiers.begin(); i != frontiers.end(); i++)
      if(i->first->end.back() >= fseg->start)
	dest[i->first] = i->second;
  }
};

void show_entity(entity *e, const char *data, const map<entity *, frontier_choice> &fm)
{
  int sf = 0;
  int ef = e->end.size()-1;
  map<entity *, frontier_choice>::const_iterator i = fm.find(e);
  if(i != fm.end()) {
    sf = i->second.sf;
    ef = i->second.ef;
  }
  char *ebuf = new char[5*(e->end[ef] - e->start[sf])];
  escape(ebuf, data + e->start[sf], e->end[ef] - e->start[sf]);
  printf("%c:%d:%s:%s", e->hyp ? 'H' : 'R', e->depth, tag_names[e->tagid].c_str(), ebuf);
  delete[] ebuf;
}

bool nodes_are_equivalent(const align_node *an1, const align_node *an2, const segment &seg)
{
  if(an1->current_pairs.size() != an2->current_pairs.size())
    return false;

  if(an1->active_set.size() != an2->active_set.size())
    return false;


  set<entity *>::const_iterator s1 = an1->active_set.begin();
  set<entity *>::const_iterator s2 = an2->active_set.begin();

  while(s1 != an1->active_set.end()) {
    if(*s1 != *s2)
      return false;

    s1++;
    s2++;
  }

  list<pair<entity *, entity *> >::const_iterator i1 = an1->current_pairs.begin();
  list<pair<entity *, entity *> >::const_iterator i2 = an2->current_pairs.begin();
  while(i1 != an1->current_pairs.end()) {
    if(i1->first != i2->first || i1->second != i2->second)
      return false;

    i1++;
    i2++;
  }

  for(unsigned int i = 0; i != seg.entities.size(); i++) {
    entity *e = seg.entities[i];
    if(!e->hyp) {
      const frontier_choice *f1 = an1->find_frontier(e);
      const frontier_choice *f2 = an2->find_frontier(e);
      if(!f1 && !f2)
	continue;
      if(!f1 || !f2)
	return false;
      if(e->end[f1->ef] <= seg.end && e->end[f2->ef] <= seg.end)
	continue;
      if(*f1 != *f2)
	return false;
    }
  }

  return true;
}

void align(vector<segment> &segments, const char *data, map<entity *, frontier_choice> &align_frontiers)
{
  list<align_node *> current_nodes;
  current_nodes.push_back(new align_node);

  for(vector<segment>::const_iterator i = segments.begin(); i != segments.end(); i++) {
#if 0
    printf("starting on segment %d, %d nodes, (sre=%d, ent=%d)\n", int(i-segments.begin()), int(current_nodes.size()), int(i->starting_ref_entities.size()), int(i->entities.size()));
    if(true)
      for(unsigned int j=0; j != i->starting_ref_entities.size(); j++) {
	map<entity *, frontier_choice> fc;
	printf(" sre %d : %p:%d - ", j, i->starting_ref_entities[j].e, i->starting_ref_entities[j].fid);
	show_entity(i->starting_ref_entities[j].e, data, fc);
	printf("\n");
      }

    for(unsigned int j=0; j != i->entities.size(); j++) {
      entity *e =  i->entities[j];
      printf("  %d %p %s (", j, e, e->hyp ? "hyp" : "ref");
      for(unsigned int k=0; k != e->start.size(); k++) {
	if(k)
	  printf(" ");
	printf("%d", e->start[k]);
      }
      printf(")-(");
      for(unsigned int k=0; k != e->end.size(); k++) {
	if(k)
	  printf(" ");
	printf("%d", e->end[k]);
      }
      printf(")\n");
    }
#endif

    assert(current_nodes.size());

    // Open new cases
    list<align_node *> opened_nodes;

    // Expand all current nodes
    for(list<align_node *>::const_iterator j = current_nodes.begin(); j != current_nodes.end(); j++) {
      align_node *pan = *j;

#if 0
      for(map<entity *, frontier_choice>::const_iterator k = pan->frontiers.begin(); k != pan->frontiers.end(); k++) {
	printf("node %p frontier %p %d %d\n", pan, k->first, k->second.sf, k->second.ef);
	if(k->second.sf == -1) {
	  printf("frontier error %p %d %d\n", k->first, k->second.sf, k->second.ef);
	  abort();
	}
      }
#endif

      // Check which reference entities should not start here because
      // they have been instantiated before for this search node.
      vector<bool> already_instanciated;
      already_instanciated.resize(i->starting_ref_entities.size());
      for(unsigned int k=0; k != i->starting_ref_entities.size(); k++)
	already_instanciated[k] = pan->find_frontier(i->starting_ref_entities[k].e) != NULL;

      // Enumerate all the acceptable combinations of reference
      // entities starting at the beginning of the segment
      map<entity *, frontier_choice> choices;

      int slot = 0;
      bool backtracking = false;
      goto initial_advance;

      for(;;) {
	slot = i->starting_ref_entities.size() - 1;
	backtracking = true;

      initial_advance:
	for(;;) {
	  //	  printf("slot %d back=%s already=%s\n", slot, backtracking ? "true" : "false", slot==-1 || slot==int(i->starting_ref_entities.size()) ? "-" : already_instanciated[slot] ? "true" : "false");

	  // try a (new) ending frontier, starting by not selecting the entity (ef=-1)
	  // backtrack if it fails, advance if it doesn't
	  // stop when running out of slots

	  // Trying to backtrack once the slot is before the start means we're done
	  if(slot == -1)
	    goto done;

	  // Trying to advance once the slot is past the end means we're found a valid combination
	  if(slot == int(i->starting_ref_entities.size()))
	    goto found_one;

	  // Skip already instantiated reference entities
	  if(already_instanciated[slot]) {
	    if(backtracking)
	      slot--;
	    else
	      slot++;
	    continue;
	  }

	  entity *e = i->starting_ref_entities[slot].e;
	  map<entity *, frontier_choice>::iterator k = choices.find(e);
	  if(k == choices.end()) {
	    // No choice yet on this slot, start by not selecting the entity
	    choices[e] = frontier_choice(i->starting_ref_entities[slot].fid, -1);

	    //	    printf("slot %d start %p, frontier=%d/%d\n", slot, e, i->starting_ref_entities[slot].fid, int(e->start.size()));

	    // Not selecting the entity is only actually acceptable
	    // if this is not the last possible segment for mapping.
	    // In the latter case, just loop on the slot without
	    // moving it to go to the first acceptable ending
	    // frontier.
	    if(i->starting_ref_entities[slot].fid == e->start.size()-1)
	      continue;

	    backtracking = false;

	  } else {
	    // Got to the next possible ending frontier, not
	    // selected (-1) is naturally followed by the first
	    // frontier (0)
	    k->second.ef++;
	    //	    printf("slot %d advancing %p, end frontier=%d/%d\n", slot, e, k->second.ef, int(e->end.size()));

	    // Out of frontiers, time to backtrack
	    if(k->second.ef == int(e->end.size())) {
	      choices.erase(k);
	      slot--;
	      backtracking = true;
	      continue;
	    }

	    backtracking = false;

#if 0
	    printf("slot %d %p frontiers %d %d\n", slot, e, e->start[k->second.sf], e->end[k->second.ef]);
	    printf("slot %d %p parent=%p left=%p\n", slot, e, e->parent, e->left_constraint);
#endif

	    // Now check whether the choice is acceptable.  On
	    // failure just loop on the slot to try the next one.

	    // First test, the entity size.  The frontiers have to
	    // be separated by at least one character.
	    if(e->start[k->second.sf] > e->end[k->second.ef])
	      continue;

	    // Second test, the parent.  If it exists, it must be
	    // instanciated and the current instance must be within
	    // it.
	    if(e->parent) {
	      const frontier_choice *l = pan->find_frontier(e->parent);
	      if(!l) {
		map<entity *, frontier_choice>::const_iterator ll = choices.find(e->parent);
		if(ll == choices.end() || ll->second.ef == -1) {
		  //		  printf("slot %d %p parent not instanciated\n", slot, e);
		  continue;
		}
		l = &ll->second;
	      }
#if 0
	      printf("slot %d %p parent frontiers %d %d\n", slot, e, e->parent->start[l->sf], e->parent->end[l->ef]);
	      printf("%d %d - %d %d\n", l->sf, l->ef, int(e->parent->start.size()), int(e->parent->end.size()));
#endif

	      // Instantiation found, check the inclusion
	      if(e->parent->start[l->sf] > e->start[k->second.sf] || e->parent->end[l->ef] < e->end[k->second.ef])
		continue;
	    }

	    // Third test, the left constraint.  If it exists, it
	    // must be instanciated and the current instance must be
	    // to the left of the current entity (they can touch).
	    if(e->left_constraint && e->left_constraint->end.back() > e->start[k->second.sf]) {
	      const frontier_choice *l = pan->find_frontier(e->left_constraint);
	      if(!l) {
		map<entity *, frontier_choice>::const_iterator ll = choices.find(e->left_constraint);
		if(ll == choices.end() || ll->second.ef == -1) {
		  //		  printf("slot %d %p left not instanciated\n", slot, e);
		  continue;
		}
		l = &ll->second;
	      }
	      //	      printf("slot %d %p left frontiers %d %d\n", slot, e, e->left_constraint->start[l->sf], e->left_constraint->end[l->ef]);

	      // Instantiation found, check the placement
	      if(e->left_constraint->end[l->ef] > e->start[k->second.sf])
		continue;
	    }

	    // All checks passed, we can go on.
	  }
	  slot++;
	}

      found_one:

	// Build a list of mappings to try
	// Count the permutations while we're at it
	list<entity *> starting_entities;
	list<vector<entity *> > target_entities;
	unsigned int nalt = 1;

	// First add the reference entities starting here associated
	// to the possible hyp entities.
	for(map<entity *, frontier_choice>::const_iterator k = choices.begin(); k != choices.end(); k++) {
	  // Skip the uninstatiated ones
	  if(k->second.ef == -1)
	    continue;

	  // Add the entity to the list
	  starting_entities.push_back(k->first);
	  target_entities.resize(target_entities.size()+1);

	  // Pick up its frontiers
	  int start = k->first->start[k->second.sf];
	  int end = k->first->end[k->second.ef];

	  // Scan the hypothesis entities to find the compatible ones
	  for(unsigned int l=0; l != i->entities.size(); l++) {
	    entity *e = i->entities[l];
	    //	    printf("scanning starting ref %d-%d vs. %s %d-%d\n", start, end, e->hyp ? "hyp" : "ref", e->start.front(), e->end.back());
	    if(e->hyp && e->start.front() < end && e->end.back() > start)
	      target_entities.back().push_back(e);
	  }

	  // Incrementally compute the permutations count
	  nalt *= 1+target_entities.back().size();
	}

	// Then add the hypothesis entities starting here associated
	// to the possible, but not starting, reference entities.
	for(unsigned int k=0; k != i->starting_hyp_entities.size(); k++) {
	  // Add the entity to the list
	  starting_entities.push_back(i->starting_hyp_entities[k]);
	  target_entities.resize(target_entities.size()+1);

	  // Scan the reference entities to find the compatible ones.
	  // They have to be instanciated in search node to be
	  // expanded (i.e. not starting in this segment), and with
	  // the end frontier after the hypothesis start (which is the
	  // segment start).
	  for(unsigned int l=0; l != i->entities.size(); l++) {
	    entity *e = i->entities[l];
	    if(!e->hyp) {
	      const frontier_choice *m = pan->find_frontier(e);
	      if(m && e->end[m->ef] > i->start)
		target_entities.back().push_back(e);
	    }
	  }

	  // Incrementally compute the permutations count
	  nalt *= 1+target_entities.back().size();
	}

	//	printf("scan done, se=%d, nalt=%d\n", int(starting_entities.size()), nalt);

	// Create a new node for every combination if it doesn't break the constraints
	for(unsigned int k=0; k<nalt; k++) {
	  align_node *an = new align_node(&*i, pan);

	  // Start by copying the already existing pairs and active
	  // vector
	  an->active_set = pan->active_set;
	  an->current_pairs = pan->current_pairs;

	  // Add the frontier instantiations
	  for(map<entity *, frontier_choice>::const_iterator l = choices.begin(); l != choices.end(); l++) {
	    // Add the instatiated ones
	    if(l->second.ef != -1) {
	      //	      printf("node add %p frontier %p %d %d\n", an, l->first, l->second.sf, l->second.ef);
	      an->add_frontier(l->first, l->second);
	    }
	  }

	  // Then do the mappings
	  int idx = k;
	  list<entity *>::iterator sei = starting_entities.begin();
	  list<vector<entity *> >::iterator tei = target_entities.begin();

	  while(sei != starting_entities.end()) {
	    int tidx;
	    if(tei->size()) {
	      int div = tei->size()+1;
	      tidx = idx % div;
	      idx /= div;
	    } else
	      tidx = 0;

	    if(!tidx) {
	      //   Don't map the entity to anything, or the entity is already used (due to previous mappings in the same segment)
	      if(!an->active(*sei)) {
		//     Score increment is equal to the entity cost
		entity *e = *sei;
		an->unmapped_entities.push_back(e);
		if(e->hyp)
		  an->score += e->miss_errors[0][0].cost;
		else {
		  const frontier_choice &ff = choices[e];
		  an->score += e->miss_errors[ff.sf][ff.ef].cost;
		}
	      }
	    } else {
	      //   Map two entities
	      //     Find the two entities ids to map
	      entity *eh = *sei;
	      entity *er = (*tei)[tidx-1];
	      if(er->hyp) {
		entity *ee = eh;
		eh = er;
		er = ee;
	      }

	      //     Skip if one is in use
	      if(an->active(er) || an->active(eh))
		goto rejected;

	      //     Test the ordering constraint, drop the combination if it fails
	      int edh = eh->depth;
	      int edr = er->depth;
	    
	      for(list<pair<entity *, entity *> >::const_iterator m = an->current_pairs.begin(); m != an->current_pairs.end(); m++) {
		int pdh = m->first->depth;
		int pdr = m->second->depth;
		if(edh < pdh && edr > pdr)
		  goto rejected;
		if(edh > pdh && edr < pdr)
		  goto rejected;
	      }

	      const frontier_choice *erf = an->find_frontier(er);
	      error_d *err = &er->subst_errors[eh][erf->sf][erf->ef];
	      if(err->cost == -1) {
		printf("seg: (%d, %d)\n", i->start, i->end);
		printf("er: %p (%d, %d)\n", er, er->start[erf->sf], er->end[erf->ef]);
		printf("eh: %p (%d, %d)\n", eh, eh->start.front(), eh->end.back());
	      }

	      assert(err->cost != -1);
	      an->added_pairs.push_back(segment::pairinfo(er, eh, err));
	      an->current_pairs.push_back(pair<entity *, entity *>(eh, er));
	      an->active_set.insert(eh);
	      an->active_set.insert(er);
	      an->score += err->cost;
	    }	    

	    sei++;
	    tei++;
	  }
	  opened_nodes.push_back(an);

#if 0
	  if(v) {
	    printf("New opened node %p (%d), score=%g, frontiers=%d\n", an, int(opened_nodes.size()), an->score, int(an->frontiers.size()));
	    printf("  active:");
	    for(set<entity *>::iterator j = an->active_set.begin(); j != an->active_set.end(); j++) {
	      printf(" ");
	      show_entity(*j, data, an->frontiers);
	    }
	    printf("\n");
	    printf("  pairs:");
	    for(list<segment::pairinfo>::const_iterator l = an->added_pairs.begin(); l != an->added_pairs.end(); l++) {
	      printf(" ");
	      show_entity(l->er, data, an->frontiers);
	      printf(" = ");
	      show_entity(l->eh, data, an->frontiers);
	    }
	    printf("\n");
	    printf("  unmapped:");
	    for(list<entity *>::const_iterator l = an->unmapped_entities.begin(); l != an->unmapped_entities.end(); l++) {
	      printf(" ");
	      show_entity(*l, data, an->frontiers);
	    }
	    printf("\n");
	  }
#endif
	  continue;
	rejected:
	  if(an)
	    an->unref();
	}
      }
    done:
      ;
    }

#if 0
    printf("%d-%d %d opened nodes :", i->start, i->end, int(opened_nodes.size()));
    for(list<align_node *>::const_iterator j = opened_nodes.begin(); j != opened_nodes.end(); j++)
      printf(" %g", (*j)->score);
    printf("\n");
#endif

    // Clearup the current nodes
    for(list<align_node *>::const_iterator j = current_nodes.begin(); j != current_nodes.end(); j++)
      (*j)->unref();
    current_nodes.clear();

    // Close and merge
    for(list<align_node *>::const_iterator j = opened_nodes.begin(); j != opened_nodes.end(); j++) {
      align_node *an = *j;
      //   Close all entities in current_pairs or active_vector that finish in the current segment
      int elimit = i->end;
      for(unsigned int k=0; k != i->entities.size(); k++)
	if(i->entities[k]->end.back() == elimit) {
	  set<entity *>::iterator l = an->active_set.find(i->entities[k]);
	  if(l != an->active_set.end())
	    an->active_set.erase(l);
	}

      for(list<pair<entity *, entity *> >::iterator k = an->current_pairs.begin(); k != an->current_pairs.end();)
	if(!an->active(k->first) || !an->active(k->second)) {
	  list<pair<entity *, entity *> >::iterator l = k;
	  k++;
	  an->current_pairs.erase(l);
	} else
	  k++;

      //   Find if an equivalent node already exists in the current nodes
      for(list<align_node *>::iterator j = current_nodes.begin(); j != current_nodes.end(); j++)
	if(nodes_are_equivalent(an, *j, *i)) {
	  //   If yes, keep the one with the best score
	  if((*j)->score <= an->score)
	    an->unref();
	  else {
	    (*j)->unref();
	    *j = an;
	  }
	  goto node_found;
	}

      current_nodes.push_back(an);
    node_found:
      ;
    }
#if 0
    printf("%d-%d %d closed nodes :", i->start, i->end, int(current_nodes.size()));
    for(list<align_node *>::const_iterator j = current_nodes.begin(); j != current_nodes.end(); j++)
      printf(" %g", (*j)->score);
    printf("\n");
#endif
  }

  assert(current_nodes.size() == 1);
  int idx = segments.size()-1;

  align_node *an = current_nodes.front();
  do {
    an->copy_frontiers_unfiltered(align_frontiers);
    segments[idx].added_pairs = an->added_pairs;
    segments[idx].unmapped_entities = an->unmapped_entities;
    idx--;
    an = an->prev;
  } while(an->prev);

  current_nodes.front()->unref();
}

void cleanup_unmapped(vector<segment> &segments, vector<entity> &ref_entities, vector<entity> &hyp_entities)
{
  for(vector<entity>::iterator i = ref_entities.begin(); i != ref_entities.end(); i++)
    i->paired = false;
  for(vector<entity>::iterator i = hyp_entities.begin(); i != hyp_entities.end(); i++)
    i->paired = false;

  for(vector<segment>::const_iterator i = segments.begin(); i != segments.end(); i++)
    for(list<segment::pairinfo>::const_iterator j = i->added_pairs.begin(); j != i->added_pairs.end(); j++) {
      j->er->paired = true;
      j->eh->paired = true;
    }

  for(vector<segment>::iterator i = segments.begin(); i != segments.end(); i++)
    for(list<entity *>::iterator j = i->unmapped_entities.begin(); j != i->unmapped_entities.end();)
      if((*j)->paired) {
	list<entity *>::iterator k = j;
	j++;
	i->unmapped_entities.erase(k);
      } else
	j++;
}

void show_entities(const vector<entity> &entities, const char *data)
{
  for(unsigned int i = 0; i != entities.size(); i++) {
    const entity &e = entities[i];
    char *ebuf = new char[5*(e.end.back() - e.start.front())];
    escape(ebuf, data + e.start.front(), e.end.back() - e.start.front());
    printf("%4d: %5d %5d %d %c %s %s\n", i, e.start.front(), e.end.back(), e.depth, e.hyp ? 'H' : 'R', tag_names[e.tagid].c_str(), ebuf);
    delete[] ebuf;
  }
}

void show_segments(const vector<segment> &segments, const char *data)
{
  for(unsigned int i=0; i != segments.size(); i++) {
    const segment &s = segments[i];
    printf("%4d: %5d %5d", i, s.start, s.end);
    for(unsigned int j=0; j != s.entities.size(); j++) {
      const entity *e = s.entities[j];
      char *ebuf = new char[5*(e->end.back() - e->start.front())];
      escape(ebuf, data + e->start.front(), e->end.back() - e->start.front());
      if(j)
	printf(" | ");
      else
	printf(" ");
      printf("%c:%d:%d %s %s", e->hyp ? 'H' : 'R', e->depth, j, tag_names[e->tagid].c_str(), ebuf);
      delete[] ebuf;
    }
    printf("\n");
  }
}

string build_error_string(const error_d &err)
{
  string s;
  for(list<int>::const_iterator i = err.error_types.begin(); i != err.error_types.end(); i++) {
    if(i != err.error_types.begin())
      s += ' ';
    s += error_names[*i];
  }
  return s;
}

void show_entity(const entity *e, const char *data, char error, const map<entity *, frontier_choice> &fm)
{
  char *ebuf;
  if(e->start.size() != 1 || e->end.size() != 1) {
    int sf = 0, ef = e->end.size()-1;
    map<entity *, frontier_choice>::const_iterator fi = fm.find(const_cast<entity *>(e));
    if(fi != fm.end()) {
      sf = fi->second.sf;
      ef = fi->second.ef;
    }
    
    string ff;
    map<int, int> fr;
    for(vector<int>::const_iterator i = e->start.begin(); i != e->start.end(); i++)
      fr[*i] |= 1;
    for(vector<int>::const_iterator i = e->end.begin(); i != e->end.end(); i++)
      fr[*i] |= 2;
    int pos = -1;
    for(map<int, int>::const_iterator i = fr.begin(); i != fr.end(); i++) {
      if(pos != -1)
	ff += string(data+pos, data+i->first);
      pos = i->first;
      if(i->second & 2)
	ff += pos == e->end[ef] ? '}' : ']';
      if(i->second & 1)
	ff += pos == e->start[sf] ? '{' : '[';
    }
    ebuf = new char[5*ff.size()];
    escape(ebuf, ff.data(), ff.size());

  } else {
    ebuf = new char[5*(e->end.back() - e->start.front())];
    escape(ebuf, data + e->start.front(), e->end.back() - e->start.front());
  }

  string ee = tag_names[e->tagid].c_str();

  if(!e->attr.empty()) {
    ee += " (";
    for(list<pair<string, string> >::const_iterator i = e->attr.begin(); i != e->attr.end(); i++) {
      if(i != e->attr.begin())
	ee += ' ';
      ee += i->first + '=' + i->second;
    }
    ee += ')';
  }
  printf("%c: %s: %s - %s\n", error, e->hyp ? "hyp" : "ref", ee.c_str(), ebuf);
  delete[] ebuf;
}

void show_details(const vector<segment> &segments, const char *data, const map<entity *, frontier_choice> &fm, const char *rfname, const char *hfname)
{
  for(vector<segment>::const_iterator i = segments.begin(); i != segments.end(); i++) {
    for(list<entity *>::const_iterator j = i->unmapped_entities.begin(); j != i->unmapped_entities.end(); j++) {
      const entity *e = *j;
      char err = e->hyp ? 'I' : 'D';
      printf("%c: %s (%g): %s:%d\n",
	     err,
	     build_error_string(e->miss_errors[0][0]).c_str(),
	     e->miss_errors[0][0].cost,
	     e->hyp ? hfname : rfname,
	     e->line);
      show_entity(e, data, err, fm);
      printf("\n");
    }

    for(list<segment::pairinfo>::const_iterator j = i->added_pairs.begin(); j != i->added_pairs.end(); j++) {
      const entity *e1 = j->er;
      const entity *e2 = j->eh;
      char err = 0;
      if(j->error->error_types.empty()) {
	if(opt_details_correct)
	  err = 'C';
      } else
	err = 'S';

      if(err) {
	printf("%c: %s (%g): %s:%d %s:%d\n",
	       err,
	       err == 'C' ? "correct" : build_error_string(*j->error).c_str(),
	       j->error->cost,
	       rfname, e1->line,
	       hfname, e2->line);
	show_entity(e1, data, err, fm);
	show_entity(e2, data, err, fm);
	printf("\n");
      }
    }    
  }    
}

void calc_scores(const vector<segment> &segments, int &tc, vector<int> &tag_hypcount, vector<int> &tag_refcount, vector<int> &tag_correct, double &ser, int &count_insert, int &count_delete, int &count_subst, int &count_correct, int &count_total)
{
  tc = tag_names.size();
  tag_hypcount.resize(tc);
  tag_refcount.resize(tc);
  tag_correct.resize(tc);

  ser = 0;
  count_insert = 0;
  count_delete = 0;
  count_subst = 0;
  count_correct = 0;
  for(vector<segment>::const_iterator i = segments.begin(); i != segments.end(); i++) {
    for(list<entity *>::const_iterator j = i->unmapped_entities.begin(); j != i->unmapped_entities.end(); j++) {
      const entity *e = *j;
      if(e->hyp) {
	count_insert++;
	tag_hypcount[e->tagid]++;
      } else {
	count_delete++;
	tag_refcount[e->tagid]++;
      }
      ser += e->miss_errors[0][0].cost;
    }

    for(list<segment::pairinfo>::const_iterator j = i->added_pairs.begin(); j != i->added_pairs.end(); j++) {
      const entity *er = j->er;
      const entity *eh = j->eh;
      tag_refcount[er->tagid]++;
      tag_hypcount[eh->tagid]++;
      if(j->error->error_types.empty()) {
	count_correct++;
	tag_correct[er->tagid]++;
      } else
	count_subst++;
      ser += j->error->cost;
    }
  }

  count_total = count_insert + count_delete + count_subst;
}

void show_summary(const vector<segment> &segments, int count_ref, int count_hyp)
{
  int tc, count_insert, count_delete, count_subst, count_correct, count_total;
  double ser;
  vector<int> tag_hypcount, tag_refcount, tag_correct;
 
  calc_scores(segments, tc, tag_hypcount, tag_refcount, tag_correct, ser, count_insert, count_delete, count_subst, count_correct, count_total);

  printf("Slot Error Rate: %5.1f%% (%g %d)\n\n", ser*100.0/count_ref, ser, count_ref);

  printf("%6d %5.1f%% corrects\n", count_correct, count_correct*100.0/count_ref);
  printf("%6d %5.1f%% inserts\n", count_insert, count_insert*100.0/count_ref);
  printf("%6d %5.1f%% deletes\n", count_delete, count_delete*100.0/count_ref);
  printf("%6d %5.1f%% substitutions\n", count_subst, count_subst*100.0/count_ref);
  printf("%6d %5.1f%% total errors\n\n", count_total, count_total*100.0/count_ref);
  if(count_hyp)
    printf("%5.1f%% overall precision (%d entities in hypothesis)\n", count_correct*100.0/count_hyp, count_hyp);
  else
    printf("   0.0%% overall precision (0 entities in hypothesis)\n");
  printf("%5.1f%% overall recall (%d entities in reference)\n", count_correct*100.0/count_ref, count_ref);
  printf("%5.1f%% overall F-measure\n\n", 2*count_correct*100.0/(count_ref+count_hyp));  

  printf("   P      R      F   tag\n");
  for(int i=0; i != tc; i++) {
    double cc = 100*tag_correct[i];
    if(tag_hypcount[i] + tag_refcount[i])
      printf("%5.1f%% %5.1f%% %5.1f%% %s (hyp_count=%d, ref_count=%d, correct=%d)\n",
	     tag_hypcount[i] ? cc/tag_hypcount[i] : 0,
	     tag_refcount[i] ? cc/tag_refcount[i] : 0,
	     tag_hypcount[i] + tag_refcount[i] ? 2*cc/(tag_hypcount[i] + tag_refcount[i]) : 0,
	     tag_names[i].c_str(),
	     tag_hypcount[i], tag_refcount[i], tag_correct[i]
	     );
  }
}

/*

Coef = (a0-ae)/(1-ae)

A0 = correct / total = (rc + ovc)/(rt + ovc)

AeS = 1/|T| (-> tend vers 1, rien a faire)

AePi = 1/(2*total)^2 * sum((correct(tag, a1) + correct(tag, a2))^2)

AeKappa = 1/(total_a*total_b) * sum(correct(tag,a1)*correct(tag,a2))

SumPi = sum((correct(tag, a1) + correct(tag, a2))^2)
SumKappa = sum(correct(tag,a1)*correct(tag,a2))

AePi = 1/(2*total)^2 * (SumPi + (va1+va2+2*ovc)^2)

AeKappa = 1/((rt1+va1+ovc)*(rt2+va2+ovc)) * (SumKappa + (va1+ovc)*(va2+ovc))

*/

void show_iag(const vector<segment> &segments, int count_ref, int count_hyp)
{
  int tc, count_insert, count_delete, count_subst, count_correct, count_total;
  double ser;
  vector<int> tag_hypcount, tag_refcount, tag_correct;
 
  calc_scores(segments, tc, tag_hypcount, tag_refcount, tag_correct, ser, count_insert, count_delete, count_subst, count_correct, count_total);

  double void_hyp, void_ref, rt;
  if(opt_open) {
    void_hyp = count_ref - count_correct;
    void_ref = count_hyp - count_correct;
    rt = count_correct + void_hyp + void_ref;

  } else {
    void_hyp = count_ref - count_correct - count_subst;
    void_ref = count_hyp - count_correct - count_subst;
    rt = count_correct + count_subst + void_hyp + void_ref;
  }
  double ovc = opt_expected_count - rt;
  if(ovc<0)
    ovc = 0;

  double A0 = ovc ? (1+count_correct/ovc)/(1+rt/ovc) : count_correct/rt;
  double AeS = 1/double(tc+1);
  double r_S = (A0-AeS)/(1-AeS);

  double SigmaPi = (void_hyp+void_ref)*(void_hyp+void_ref);
  double SigmaKappa = void_hyp*void_ref;

  for(int i=0; i<tc; i++) {
    double cpi = double(tag_hypcount[i]+tag_refcount[i]);
    SigmaPi += cpi*cpi;
    SigmaKappa += double(tag_hypcount[i])*double(tag_refcount[i]);
  }

  double r_pi, r_kappa;

  if(ovc) {
    r_pi = (8*count_correct-4*(rt-count_correct) + (4*count_correct*rt - SigmaPi)/ovc)/(8*count_correct + (4*rt*rt - SigmaPi)/ovc);
    r_kappa = (2*count_correct + (count_correct*rt - SigmaKappa)/ovc)/(count_correct+rt + (rt*rt - SigmaKappa)/ovc);
  } else {
    r_pi = (4*count_correct*rt - SigmaPi)/(4*rt*rt - SigmaPi);
    r_kappa = (count_correct*rt - SigmaKappa)/(rt*rt - SigmaKappa);
  }

  double r_fm = 2*count_correct/double(count_ref + count_hyp);

  printf("Total entities: %d\n", int(rt));
  printf("Correct: %d\n", count_correct);
  printf("Added void/void corrects: %g\n", ovc);
  printf("Tag types: %d\n", tc);
  printf("\n");
  printf("S         = %7.5f\n", r_S);
  printf("Pi        = %7.5f\n", r_pi);
  printf("Kappa     = %7.5f\n", r_kappa);
  printf("F-measure = %7.5f\n", r_fm);
}


// Option handling
static void print_usage(ostream &out)
{
  out << "NE scoring\n"
      << "\n"
      << "Usage: " << progname << " [options] descr.lua ref-file hyp-file\n"
      << "  -a                  reference is in \"aref\" format\n"
      << "  -s                  show summary of results (default)\n"
      << "  -d                  show detail of errors\n"
      << "  -c                  show detail of errors and corrects\n"
      << "  -i <expected_count> show IAG-type values\n"
      << "  -o                  open - in IAG mode, there are no confusions\n"
      << "\n"
      << endl;
}

static void options(int argc, char ***argv)
{
  static option optlist[] = {
    { "help", 0, 0, 'h' },
    { 0,      0, 0,  0  }
  };

  int usage = 0, finish = 0, error = 0;

  opt_summary = opt_details = opt_details_correct = opt_iag = opt_ref_aref = opt_open = false;
  opt_expected_count = 0;

  for(;;) {
    int opt = getopt_long(argc, *argv, "hasdci:o", optlist, 0);
    if(opt == EOF)
      break;
    switch(opt) {
    case 'h':
      usage = 1;
      finish = 1;
      error = 0;
      break;
    case 'a':
      opt_ref_aref = true;
      break;
    case 's':
      opt_summary = true;
      break;
    case 'd':
      opt_details = true;
      break;
    case 'i':
      opt_iag = true;
      opt_expected_count = strtod(optarg, 0);
      break;
    case 'c':
      opt_details = opt_details_correct = true;
      break;
    case 'o':
      opt_open = true;
      break;
    case '?':
    case ':':
      usage = 1;
      finish = 1;
      error = 1;
      break;
    default:
      abort();
    }
    if(finish)
      break;
  }
  if(usage)
    print_usage(error ? cerr : cout);
  if(finish)
    exit(error);

  if(!opt_summary && !opt_details && !opt_iag)
    opt_summary = true;

  *argv += optind;
}

int main(int argc, char **argv)
{
  progname = argv[0];

  options(argc, &argv);

  if(!argv[0] || !argv[1] || !argv[2] || argv[3]) {
    print_usage(cerr);
    exit(1);
  }

  char *ref_data, *hyp_data;
  list<simple_tag> ref_stags, hyp_tags;
  list<aref_tag> ref_atags;
  vector<entity> ref_ents, hyp_ents;
  map<int, list<entity *> > frontiers;
  vector<segment> segments;
  map<entity *, frontier_choice> align_frontiers;

  lua_State *L = luaL_newstate();

  load_lua_description(L, argv[0]);
  load_tag_list(L);

  tag_hypcount.resize(tag_names.size());
  tag_refcount.resize(tag_names.size());
  tag_correct.resize(tag_names.size());

  annotated_file_load(argv[2], hyp_tags, hyp_data);

  if(opt_ref_aref) {
    aref_file_load(argv[1], ref_atags, ref_data);
    build_entities_from_tags(ref_ents, ref_atags, argv[1], false);
  } else {
    annotated_file_load(argv[1], ref_stags, ref_data);
    build_entities_from_tags(ref_ents, ref_stags, argv[1], false);
  }

  align_and_reposition(ref_data, hyp_data, hyp_tags);

  // From that point hyp_tags (->hyp_ents) refers to ref_data, *not* hyp_data

  build_entities_from_tags(hyp_ents, hyp_tags, argv[2], true);

  refine_entities(ref_ents, ref_data, argv[1]);
  refine_entities(hyp_ents, ref_data, argv[2]); // *not* hyp_data due to align_and_reposition

  compute_entities_miss_costs(L, ref_ents, ref_data);
  compute_entities_miss_costs(L, hyp_ents, ref_data);

  //  show_entities(ref_ents, ref_data);
  //  show_entities(hyp_ents, ref_data);

  add_frontiers(frontiers, ref_ents);
  add_frontiers(frontiers, hyp_ents);

  build_segments(segments, frontiers);
  //  show_segments(segments, ref_data);

  compute_substitution_errors_costs(L, segments, ref_data);

  align(segments, ref_data, align_frontiers);
  cleanup_unmapped(segments, ref_ents, hyp_ents);

  if(opt_details)
    show_details(segments, ref_data, align_frontiers, argv[1], argv[2]);

  if(opt_summary)
    show_summary(segments, ref_ents.size(), hyp_ents.size());

  if(opt_iag)
    show_iag(segments, ref_ents.size(), hyp_ents.size());

  lua_close(L);

  return 0;
}
