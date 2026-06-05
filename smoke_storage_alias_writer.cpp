#include "wvz4_writer_typed.h"
#include <iostream>
int main(){
  wvz4::Layout l;
  l.names.push_back({1,"top"}); l.names.push_back({2,"a"}); l.names.push_back({3,"b"});
  wvz4::NodeRecord root; root.node_id=1; root.parent_id=0; root.name_id=1; root.kind=wvz4::NodeKind::Object; root.first_child=2; root.next_sibling=0; l.nodes.push_back(root);
  wvz4::NodeRecord a; a.node_id=2; a.parent_id=1; a.name_id=2; a.kind=wvz4::NodeKind::SignalLeaf; a.first_child=0; a.next_sibling=3; l.nodes.push_back(a);
  wvz4::NodeRecord b; b.node_id=3; b.parent_id=1; b.name_id=3; b.kind=wvz4::NodeKind::SignalLeaf; b.first_child=0; b.next_sibling=0; l.nodes.push_back(b);
  wvz4::SignalDefinition s1; s1.signal_id=1; s1.storage_id=1; s1.node_id=2; s1.type=wvz4::ValueType::Bool; s1.bit_width=1; s1.radix=wvz4::Radix::Bin; l.signals.push_back(s1);
  wvz4::SignalDefinition s2=s1; s2.signal_id=2; s2.storage_id=1; s2.node_id=3; l.signals.push_back(s2);
  wvz4::WriterOptions opt; opt.compression=wvz4::Compression::None; opt.enable_stats_log=false; opt.target_block_span=10;
  wvz4::Writer w; std::string err;
  if(!w.open("/tmp/alias_storage.wvz4", l, opt, err)){std::cerr<<err<<"\n"; return 1;}
  wvz4::CycleSubmission c; c.cycle=1; c.updates.push_back(wvz4::CycleValueUpdate::make_bool(1,true));
  if(!w.submit_cycle(c, err)){std::cerr<<err<<"\n"; return 2;}
  if(!w.close(err)){std::cerr<<err<<"\n"; return 3;}
  return 0;
}
