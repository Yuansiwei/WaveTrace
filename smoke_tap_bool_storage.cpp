#include "wave_tap.h"
#include "wave_path_wvz4_recorder.h"
#include <iostream>
typedef unsigned char U01;
struct Top { U01 flag; unsigned char data; };
namespace reflect {
template<> struct is_reflected<Top> : std::true_type {};
template<> struct reflected_visitor<Top> {
  template<class P,class V,class G>
  static void visit(const Top* obj, P&& on_ptr, V&&, G&&) {
    on_ptr("flag", ::wave::as_bool_storage_ptr(std::addressof(obj->flag)));
    on_ptr("data", std::addressof(obj->data));
  }
};
}
int main(){
  Top top{0,0}; std::string err;
  PathStableWvz4Recorder rec; PathStableWvz4Recorder::OpenConfig cfg; cfg.file_path="/tmp/tap_bool_storage.wvz4"; cfg.async_writer=false; cfg.emit_default_clk=false; cfg.options.compression=wvz4::Compression::None; cfg.options.enable_stats_log=false; cfg.options.target_block_span=10;
  if(!rec.open(cfg,err)){std::cerr<<err<<"\n"; return 1;}
  wave::BuildOptions opt; opt.enable_flat_memory_block_precheck=true; opt.enable_flat_leaf_fast_table=true;
  wave::Tracer tracer(rec,opt); tracer.add_root("top", &top); wave::WaveTap tap(tracer,rec);
  if(!tap.sample_one_cycle()){std::cerr<<tap.last_error()<<"\n"; return 2;}
  top.flag=2; top.data=7;
  if(!tap.sample_one_cycle()){std::cerr<<tap.last_error()<<"\n"; return 3;}
  if(!rec.close(err)){std::cerr<<err<<"\n"; return 4;}
  return 0;
}
