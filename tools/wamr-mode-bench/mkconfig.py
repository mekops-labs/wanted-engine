import json,sys
screen,mode,hz,secs=sys.argv[1:5]
print(json.dumps({
 "system":{"screens":[
   {"name":"term","width":480,"height":222,"format":"rgb565"},
   {"name":"tender","width":540,"height":960,"format":"gray4"}]},
 "supervisor":{"imagePath":"wlkload.tar","params":{
   "privileged":True,
   "console":{"in":{"name":"null"},"out":{"name":"platform"},"err":{"name":"platform"}},
   "envs":[f"SCREEN={screen}",f"MODE={mode}",f"RATE_HZ={hz}",f"SECONDS={secs}","WARMUP_S=4"],
   "drivers":[{"name":"wanted"},{"name":"fb","options":f"screens={screen}"}]}}},indent=1))
