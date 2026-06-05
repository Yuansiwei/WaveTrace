#include "wave_runtime.h"
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
  Top t{0,0};
  wave::InMemoryWaveSink sink;
  wave::BuildOptions opt;
  opt.emit_track_decl_path=true;
  wave::Tracer tr(sink,opt);
  tr.add_root("top", &t);
  tr.sample(0);
  t.flag=2; t.data=2;
  tr.sample(1);
  if(sink.declarations.size()!=2){std::cerr<<"decls "<<sink.declarations.size()<<"\n";return 1;}
  bool saw_bool=false,saw_u8=false;
  for(auto &d:sink.declarations){ if(d.path.find("flag")!=std::string::npos && d.kind==wave::ValueKind::Bool && d.bit_width==1) saw_bool=true; if(d.path.find("data")!=std::string::npos && d.kind==wave::ValueKind::UnsignedInt && d.bit_width==8) saw_u8=true; }
  if(!saw_bool||!saw_u8){std::cerr<<"bad decl\n";return 2;}
  bool flag_true=false;
  for(auto &e:sink.events){ if(e.has_bool && e.bool_value) flag_true=true; }
  if(!flag_true){std::cerr<<"no bool true\n";return 3;}
  return 0;
}
