ERRMOD_TRIES = (ERRMOD_TRIES or 0) + 1
if ERRMOD_TRIES == 1 then error("first load fails") end
return "second load works"
