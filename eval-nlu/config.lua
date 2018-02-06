all_tags = {
   "recipe",
   "neg_cat-ingredient",
   "cat-ingredient",
   "ingredient",
   "neg_ingredient"
}

-- Interface
--   Give the list of all tags
function get_all_tags()
   return all_tags
end

--   Give the list of all component (bottom level) tags
function get_component_tags()
   return component_tags
end

--   Give the costs
--     root boundary error
--     root class error
--     root sub-class error
--     component type error
--     component frontier error
-- function get_costs()
--    return 0.25, 0.5, 0.25 --, 0.5, 0.25
-- end

--  Entity structure:
--    type  = string,  name of the tag
--    value = string,  text of the entity
--    attr  = table,   { attribute = value } pairs
--    hyp   = boolean, is the entity from the hypothesis (vs. reference)
--    spos  = integer, position of the start of the entity in the text
--    epos  = integer, position of the end of the entity in the text

--   Give the miss cost and error list for one entity
function get_miss_cost(e)
--   print(string.format("Get miss cost on %s %s %s", e.hyp and "H" or "R", e.type, e.value))
   return 1, e.hyp and "fa" or "miss"
end

--   Give the substitution cost and error list for a pair of entities
function get_substitution_cost(e1, e2)
   local c = 0
   local err = {}
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
