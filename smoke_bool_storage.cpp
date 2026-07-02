#include "wave_runtime.h"
#include <iostream>

typedef unsigned char U01;

static int g_slot_visit_count = 0;

struct Slot {
  unsigned count;
  unsigned tag;
};

struct Top {
  U01 flag;
  unsigned char data;
  Slot slots[8];
};

namespace reflect {
template<> struct is_reflected<Slot> : std::true_type {};
template<> struct reflected_visitor<Slot> {
  template<class P,class V,class G>
  static void visit(const Slot* obj, P&& on_ptr, V&&, G&&) {
    ++g_slot_visit_count;
    on_ptr("count", std::addressof(obj->count));
    on_ptr("tag", std::addressof(obj->tag));
  }
};

template<> struct is_reflected<Top> : std::true_type {};
template<> struct reflected_visitor<Top> {
  template<class P,class V,class G>
  static void visit(const Top* obj, P&& on_ptr, V&&, G&&) {
    on_ptr("flag", ::wave::as_bool_storage_ptr(std::addressof(obj->flag)));
    on_ptr("data", std::addressof(obj->data));
    on_ptr("slots", std::addressof(obj->slots));
  }
};
}
int main(){
  Top t{};
  t.flag = 0;
  t.data = 0;
  for (int i = 0; i < 8; ++i) {
    t.slots[i].count = static_cast<unsigned>(i);
    t.slots[i].tag = static_cast<unsigned>(i + 10);
  }
  wave::InMemoryWaveSink sink;
  wave::BuildOptions opt;
  opt.emit_track_decl_path=true;
  wave::Tracer tr(sink,opt);
  tr.add_root("top", &t);
  tr.sample(0);
  if(g_slot_visit_count!=8){std::cerr<<"slot topology visits "<<g_slot_visit_count<<"\n";return 4;}
  t.flag=2; t.data=2;
  t.slots[3].count=33;
  tr.sample(1);
  bool saw_bool=false,saw_u8=false,saw_slot=false;
  wave::TrackId slot_track=0;
  for(auto &d:sink.declarations){
    if(d.path.find("flag")!=std::string::npos && d.kind==wave::ValueKind::Bool && d.bit_width==1) saw_bool=true;
    if(d.path.find("data")!=std::string::npos && d.kind==wave::ValueKind::UnsignedInt && d.bit_width==8) saw_u8=true;
    if(d.path=="top.slots.[3].count" && d.kind==wave::ValueKind::UnsignedInt){ saw_slot=true; slot_track=d.track_id; }
  }
  if(!saw_bool||!saw_u8||!saw_slot){std::cerr<<"bad decl\n";return 2;}
  bool flag_true=false, slot_changed=false;
  for(auto &e:sink.events){
    if(e.has_bool && e.bool_value) flag_true=true;
    if(e.track_id==slot_track && e.cycle==1 && e.has_u64 && e.u64_value==33) slot_changed=true;
  }
  if(!flag_true){std::cerr<<"no bool true\n";return 3;}
  if(!slot_changed){std::cerr<<"no slot update\n";return 5;}
  return 0;
}
