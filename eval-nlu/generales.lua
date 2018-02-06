types = {
   pers = { "ind", "coll", "other", "unk" },
   func = { "ind", "coll", "other", "unk" },
   org  = { "ent", "adm", "other", "unk" },
   loc  = {
      adm  = { "town", "reg", "nat", "sup" },
      phys = { "geo", "hydro", "astro" },
      "oro", "fac",
      add =  { "phys", "elec" },
      "other", "unk" },
   prod =  { "object", "art", "media", "fin", "soft", "award", "serv", "doctr", "rule", "other", "unk" },
   "amount",
   time = { date = { "abs", "rel" }, hour = { "abs", "rel" }, "other", "unk" },
   "event",
   "noisy-entities"
}

components = {
   "kind", "extractor", "qualifier", demonym = { "nickname"}, name = { "last", "first", "middle", "nickname" },
   "title",
   "address-number", "po-box", "zip-code", "other-address-component",
   "val", "unit", "object", "range-mark",
   "day", "week", "month", "year", "century", "millenium", "reference-era", "range-mark", "time-modifier",
   "val", "unit", "range-mark", "time-modifier",
   "award-cat",
}


-- Small recursive function to rebuild the types names

function tag_scan_array(ar, prefix, dest, ok_alone)
   for k,v in pairs(ar) do
      if(type(v) == "table") then
	 if(ok_alone) then
	    dest[#dest+1] = prefix..k
	 end
	 tag_scan_array(v, prefix..k..".", dest, ok_alone)
      else
	 dest[#dest+1] = prefix..v
      end
   end
end

-- Build the list of all possible entity tags

all_tags = {}
tag_scan_array(types, "", all_tags, false)
tag_scan_array(components, "", all_tags, true)


-- Interface
--   Give the list of all tags
function get_all_tags()
   return all_tags
end


--   Entity structure:
--     type  = string,  name of the tag
--     value = string,  text of the entity
--     attr  = table,   { attribute = value } pairs
--     hyp   = boolean, is the entity from the hypothesis (vs. reference)
--     spos  = integer, position of the start of the entity in the text
--     epos  = integer, position of the end of the entity in the text

--   Give the miss cost and error list for one entity
function get_miss_cost(e)
--   print(string.format("Get miss cost on %s %s %s", e.hyp and "H" or "R", e.type, e.value))
   if(e.type == "noisy-entities") then
      return 0, "catchall"
   end
   return 1, e.hyp and "fa" or "miss"
end

--   Give the substitution cost and error list for a pair of entities
function get_substitution_cost(e1, e2)
   local c = 0
   local err = {}
   if(e1.type == "noisy-entities" or e2.type == "noisy-entities") then
      return 0, "catchall"
   end
   if(e1.spos ~= e2.spos or e1.epos ~= e2.epos) then
      c = c + 0.5
      err[#err+1] = "frontier"
   end
   if(e1.type ~= e2.type) then
      c = c + 0.5
      err[#err+1] = "type"
   end
   return c, err
end
