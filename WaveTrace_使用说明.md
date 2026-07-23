# WaveTrace 波形系统使用说明

普通用户看“省流部分”修改 JSON 即可；需要添加新的 private/protected 追踪目标时看“进阶 1”，需要添加指针目标时看“进阶 2”，需要给 module 增加超大固定数组时再看“进阶 3”。

## 省流部分：JSON 怎么用

### 文件位置

CMake 集成默认读取：
```te
WaveTracer/wavetrace_config.json
```
该文件在 Visual Studio 的 `cmodel` 工程中显示于 `configs` Filter。

### 推荐配置

```json
{
  "WaveTrace": true,
  "WaveTraceFileName": "wave.wvz4",
  "WaveTraceStart": "",
  "WaveTraceEnd": "",
  "WaveTraceLevel": ""
}
```

推荐配置中故意省略 `wave_ptr_members`：它不要求为空，也不应为了套用示例而清空。ReflectGen 会按当前业务代码自动发现并维护该表；已有非空内容时应保留，只按需修改对应条目的 `reflect`。

### 字段说明

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `WaveTrace` | `true` / `false` | 波形反射与采样总开关。改为 `true` 后需要重新构建 `cmodel`；改为 `false` 只需重启业务程序，不需要重新构建。 |
| `WaveTraceFileName` | 字符串 | 输出 `.wvz4` 文件名或路径。集成构建中以该字段为准；相对路径相对于业务程序的运行目录。空字符串按 `wave.wvz4` 处理。 |
| `WaveTraceStart` | 非负整数、数字字符串、`""` 或 `null` | 开始采样的业务 Cycle，包含该 Cycle。空值表示 `0`。 |
| `WaveTraceEnd` | 非负整数、数字字符串、`""` 或 `null` | 结束采样的业务 Cycle，包含该 Cycle。空值表示无限大，文件最终结束位置取程序实际运行终点。 |
| `WaveTraceLevel` | 非负整数、数字字符串、`""` 或 `null` | 波形路径深度上限。空值或 `0` 表示不限制；正整数限制路径中 `.` 的最大数量。 |
| `wave_ptr_members` | 数组，可省略 | ReflectGen 自动维护的指针目标反射开关，可以是非空数组，不要手工清空，见第 2 节。 |

`WaveTraceEnd` 不能小于 `WaveTraceStart`，否则配置无效，程序会报告错误而不是静默使用错误区间。

### 开启全部波形

```json
{
  "WaveTrace": true,
  "WaveTraceFileName": "gpu_full.wvz4",
  "WaveTraceStart": "",
  "WaveTraceEnd": "",
  "WaveTraceLevel": ""
}
```

从业务 Cycle 0 开始采样，结束位置取程序实际运行终点。

### 只采一段 Cycle

```json
{
  "WaveTrace": true,
  "WaveTraceFileName": "gpu_10000_20000.wvz4",
  "WaveTraceStart": 10000,
  "WaveTraceEnd": 20000,
  "WaveTraceLevel": ""
}
```

采样区间为闭区间 `[10000, 20000]`。区间外仍推进业务 Cycle，但不展开、采样或向 writer 提交波形：

```text
[sim] cycle=9999 
[wave] cycle=10000 
...
[wave] cycle=20000 
[sim] cycle=20001 
```

### 完全关闭波形

```json
{
  "WaveTrace": false,
  "WaveTraceFileName": "wave.wvz4",
  "WaveTraceStart": "",
  "WaveTraceEnd": "",
  "WaveTraceLevel": ""
}
```

关闭后：

- 改为 `false` 后只需重新启动业务程序，不需要重新构建。
- 之后改回 `true` 时需要重新构建 `cmodel`，让 ReflectGen 生成并编译完整反射代码。
- 不展开拓扑、不采样、不生成 `.wvz4` 文件。
- `WaveTap` 仍会在启动阶段和每个时钟下降沿推进 Cycle，但采样路径是成功空操作。
- 开启 Cycle 进度输出后显示 `[sim] cycle=...`，不会显示 `[wave]`。

Cycle 进度输出使用现有运行时选项：

```cpp
wave::BuildOptions options;
options.print_cycle_progress = true;
options.print_cycle_progress_period = 10; // 每 10 个业务 Cycle 输出一次
```

### 修改配置后的操作

修改 JSON 后是否需要重新构建，取决于 `WaveTrace` 总开关：

- 配置在进程内首次读取后会缓存，运行中修改文件不会热加载。
- 将 `WaveTrace` 改为 `true` 后，需要重新构建 `cmodel`；构建过程会运行 ReflectGen，并把完整反射代码编译进程序。
- 将 `WaveTrace` 改为 `false` 后，不需要重新构建，退出并重新启动业务程序即可关闭波形。
- 文件名、采样区间、层级以及 `wave_ptr_members[].reflect` 仍由新进程直接读取，修改这些选项不需要重新构建。
- `wave_ptr_members[].reflect=false` 会在首次拓扑展开时跳过该指针成员；改回 `true` 并重启程序即可恢复。

除将 `WaveTrace` 改为 `true` 外，以下 C++ 代码变化也需要重新构建：

- 新增、删除或改名 `WAVE_PTR` / `WAVE_PTR_ARRAY` 业务成员属于 C++ 代码变化，需要重新构建。

ReflectGen 运行时会规范化 JSON，并自动补充发现的指针目标成员。

## 进阶部分

### 进阶 1：追踪 private/protected 成员

默认只能生成对 public 成员的访问。如果要把某个 class 的 private 或 protected 成员加入波形，需要在该 class 的类体内写一行 `WAVE_REFLECT_FRIEND`：

```cpp
#include "reflect_macro.h"

class CmodelState {
    WAVE_REFLECT_FRIEND

private:
    std::uint32_t state_ = 0;
    bool valid_ = false;

public:
    void update(std::uint32_t state, bool valid) {
        state_ = state;
        valid_ = valid;
    }
};
```

只要 `CmodelState` 对象已经处于正常的反射对象链中，`state_` 和 `valid_` 就会与 public 成员一样加入波形。业务代码仍只能通过 class 自己的 public 接口访问这些成员。

#### 注意

- `WAVE_REFLECT_FRIEND` 必须是 class 体内的真实宏，写在注释或字符串中不会生效。
- 权限按类单独生效，不会从外层类传递给成员类型。哪个类定义了需追踪的 private/protected 成员，就在哪个类里加宏。
- 宏可以放在 `public:`、`protected:` 或 `private:` 段，通常直接放在左花括号后，避免遗漏。
- 它只授权生成反射代码访问成员，不改变原有访问权限，也不增加对象实例数据或改变对象布局。
- 添加宏、新增 private/protected 成员或修改成员名后，需要重新运行 ReflectGen 并编译新的生成代码。

### 进阶 2：追踪指针目标

#### 什么时候使用

普通指针默认不展开，避免误追踪临时对象、外部内存或不稳定的动态对象图。明确需要追踪某个指针目标时，在成员定义前加 WaveTrace 标记即可；成员仍是原生指针或标准智能指针，不改变对象布局和原有用法。

推荐使用的标记：

```cpp
WAVE_PTR T* member;
WAVE_PTR std::unique_ptr<T> member;
WAVE_PTR std::shared_ptr<T> member;
WAVE_PTR_ARRAY(count_member) T* array_member;
WAVE_PTR std::weak_ptr<T> weak_member;
```

这些宏由 `reflect_macro.h` 提供。如果标记成员本身是 private/protected，在它所属的 class 中按“进阶 1”加入 `WAVE_REFLECT_FRIEND`；如果目标类型中也有 private/protected 成员，目标 class 也要单独加宏。

#### 单对象裸指针

```cpp
#include "reflect_macro.h"

struct Node {
    std::uint32_t state = 0;
};

class Top {
public:
    WAVE_PTR Node* active_node = nullptr;
};
```

在第一次采样前设置目标：

```cpp
Node node;
Top top;
top.active_node = &node;
```

`WAVE_PTR` 表示单个对象，不需要声明长度。成员仍是普通 `Node*`，所以比较、赋值、`.->`、`delete` 等业务代码保持原样；WaveTrace 不接管裸指针所有权。

#### 运行期长度的连续数组

数组用 `WAVE_PTR_ARRAY(长度成员)` 标记。括号中写同一个对象内保存长度的成员名：

```cpp
struct Slot {
    std::uint32_t value = 0;
};

class Top {
public:
    std::size_t slot_count = 0;
    WAVE_PTR_ARRAY(slot_count) Slot* slots = nullptr;
};

std::vector<Slot> storage(4096);
Top top;
top.slots = storage.data();
top.slot_count = storage.size();
```

长度成员的值是运行期值，可以由构造函数参数、配置或计算结果赋值。ReflectGen 只接受一个整数成员名或整数常量，例如 `WAVE_PTR_ARRAY(slot_count)`、`WAVE_PTR_ARRAY(16)`；不要写局部变量、函数调用或 `count + 1`。

长度在第一次拓扑展开时读取。通常就是采样区间内第一次 `tap.sample_one_cycle()`；在此之前必须把指针和长度都设置好。`slot_count == 4096` 会展开 `slots[0]` 到 `slots[4095]`。必须保证：

- 指针指向至少 `count` 个连续、有效的 `Slot` 对象。
- 内存在整个追踪期间保持存活。
- `std::vector` 在设置指针后不能再发生扩容或搬迁。
- 指针和长度成员必须在第一次拓扑展开前设置完成。

第一次展开后会冻结拓扑。之后替换指针或改变长度不会自动重建波形树。

#### unique_ptr 和 shared_ptr

```cpp
class Top {
public:
    WAVE_PTR std::unique_ptr<Node> owned_node;
    WAVE_PTR std::shared_ptr<Node> shared_node;
};

Top top;
top.owned_node = std::unique_ptr<Node>(new Node());
top.shared_node = std::make_shared<Node>();
```

智能指针形式默认也是单对象。所有权语义仍由原智能指针保持。连续数组可以使用 `WAVE_PTR_ARRAY(count_member) std::unique_ptr<T[]>`，长度规则与裸指针数组相同。

#### weak_ptr

```cpp
class Top {
public:
    WAVE_PTR std::weak_ptr<Node> observed_node;
};
```

`weak_ptr` 不使用单独的宏；ReflectGen 会从成员的 C++ 类型自动选择安全的弱引用展开路径。

第一次拓扑展开时，WaveTrace 会对它调用 `lock()`：

- 如果目标已经失效，该成员不产生波形节点。
- 如果 `lock()` 成功，Tracer 会保存一份内部 `shared_ptr`，让目标至少存活到该 Tracer 销毁，保证后续采样地址稳定。
- 业务成员本身仍是 `std::weak_ptr`；但追踪期间目标可能因为 Tracer 的内部保活而晚一些析构。
- 第一次展开后重新绑定 `weak_ptr` 不会改变已经冻结的波形树。

#### private 指针成员的完整例子

```cpp
class Top {
    WAVE_REFLECT_FRIEND

public:
    void initialize(Node* active, Slot* slots, std::size_t count) {
        active_node_ = active;
        slots_ = slots;
        slot_count_ = count;
    }

private:
    WAVE_PTR Node* active_node_ = nullptr;
    std::size_t slot_count_ = 0;
    WAVE_PTR_ARRAY(slot_count_) Slot* slots_ = nullptr;
    WAVE_PTR std::shared_ptr<Node> owned_elsewhere_;
    WAVE_PTR std::weak_ptr<Node> observer_;
};
```

#### wave_ptr_members 如何生成

添加带标记的指针成员并构建一次。ReflectGen 会自动发现这些字段，并把 JSON 更新成类似：

```json
{
  "WaveTrace": true,
  "WaveTraceFileName": "wave.wvz4",
  "WaveTraceStart": "",
  "WaveTraceEnd": "",
  "WaveTraceLevel": "",
  "wave_ptr_members": [
    {"class": "Top", "member": "active_node", "reflect": true},
    {"class": "Top", "member": "slots", "reflect": true}
  ]
}
```

有命名空间时，`class` 使用规范化后的限定名，例如 `gpu::Top`。模板类的模板实参会被规范化掉，同一个模板成员共用一个开关。建议让 ReflectGen 自动生成条目，不要手写猜测类名。

每个条目都必须包含：

| 字段 | 含义 |
| --- | --- |
| `class` | 指针成员所属类。 |
| `member` | 成员变量名。 |
| `reflect` | 程序启动后首次展开拓扑时，是否展开该指针成员。 |

相同 `class + member` 不能重复，否则 ReflectGen 会报配置错误。

#### 按成员关闭指针目标反射

如果某个大指针目标不需要波形，将对应条目改为：

```json
{"class": "Top", "member": "slots", "reflect": false}
```

保存 JSON 后重新启动业务程序即可，不需要重新构建。`reflect=false` 会在首次拓扑展开时跳过该成员及其目标子树。

重新启用时改回 `true` 并重新启动业务程序。

`wave_ptr_members[].reflect` 是运行期选项，修改它不需要重新构建。需要注意，`WaveTrace` 总开关的规则不同：改为 `true` 后必须重新构建 `cmodel`，改为 `false` 则不需要重新构建。

#### 接入顺序检查

首次采样前完成以下步骤：

1. 构造并初始化所有业务对象。
2. 设置全部指针目标和运行期数组长度。

不要由业务代码手工组合 `prepare_topology()`、`begin_cycle()`、`Tracer::sample()` 和 `end_cycle()`；SystemC 下由 `WaveTap` 在启动阶段和注册时钟的下降沿自动采样。

#### 常见问题

**指针数组没有出现或元素数量不对**

检查 `WAVE_PTR_ARRAY(count_member)` 是否引用了正确的整数成员，以及指针和长度是否在第一次拓扑展开前设置。

**第一次采样后更换指针，Viewer 没有新对象**

这是稳定拓扑设计。第一次采样后不会重建指针目标；应在首次采样前固定目标地址和长度。

### 进阶 3：使用 wave::array 追踪超大数组

**只有要给 module 增加超大固定数组，而且大多数 Cycle 只修改少量元素时，才建议使用 `wave::array`。**

普通小数组、每个 Cycle 都整体重写的数组，或根本不需要进入波形的缓冲区，继续使用 C 数组或 `std::array`，不要机械替换。

`wave::array<T, N>` 是固定长度数组，尺寸和对齐与 `std::array<T, N>` 相同，不在对象中增加 tracer id 或其他实例数据。它的作用是在业务代码通过元素写入口访问数组时，让 WaveTrace 知道本 Cycle 需要检查哪个元素。

#### module 示例

```cpp
#include "reflect_macro.h"

class Module {
public:
    static constexpr std::size_t kEntryCount = 1u << 20;
    wave::array<std::uint32_t, kEntryCount> counters;

    void write(std::size_t index, std::uint32_t value) {
        counters[index] = value;
    }

    std::uint32_t read(std::size_t index) const {
        return counters.read(index);
    }
};
```

`counters[index] = value` 会标记对应元素需要检查。纯读取使用 const 对象的 `operator[]`、`read(index)` 或 `read()`，不会把元素标记为可能修改。

如果超大数组是 module 的 private/protected 成员，按“进阶 1”在该 module 的 class 体内加入 `WAVE_REFLECT_FRIEND`。

#### 性能注意

- 非 const `operator[]`、`at()`、`front()` 和 `back()` 会把访问到的元素标记为可能修改，即使业务代码最终没有改变它的值。
- 非 const `data()`、`begin()`、`rbegin()`、`fill()`、整数组赋值和 `swap()` 可能修改任意元素，因此会把整个数组标记为需要检查。
- 纯读遍历不要从非 const 对象取可写迭代器。可以先取 `const auto& view = counters;`，或直接使用 `counters.read()` 返回的 const 视图。
- `wave::array` 优化的是建立波形后的稀疏元素采样。它不会减少信号数量，也不会免除首次展开超大数组的成本。
- `N` 必须是编译期常量。如果长度只能在运行时确定，不要为了使用 `wave::array` 硬设一个过大上限；应根据对象生命期选择原有容器或“进阶 2”的 `WAVE_PTR_ARRAY` 连续数组方式。
