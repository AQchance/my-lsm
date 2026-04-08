# Tiny-LSM 跳表 (SkipList) 实现细节解析

在 Tiny-LSM 项目中，跳表（SkipList）作为内存表（MemTable）的核心数据结构，负责承接高并发的读写请求。以下是根据源码（`include/skiplist/skiplist.h` 和 `src/skiplist/skipList.cpp`）对跳表的核心机制（节点设计、CRUD 操作、迭代器及范围查询）的详细解析。

## 1. 核心节点设计：支持 MVCC 的 `SkipListNode`

跳表不仅存储键值对，还需要支持多版本并发控制（MVCC）。每个节点包含：
- `key_`：键。
- `value_`：值。
- `tranc_id_`：事务 ID，用于实现 MVCC。
- `forward_`：`std::vector<std::shared_ptr<SkipListNode>>`，向前指针数组（多层结构）。
- `backward_`：`std::vector<std::weak_ptr<SkipListNode>>`，向后指针数组（多层结构）。这里巧妙地使用了 `std::weak_ptr`，避免了 C++ 智能指针的双向引用导致的内存泄漏。

**多版本排序规则**：
节点的 `<` 运算符重载规定：首先比较 `key_`；当 `key_` 相同时，**`tranc_id_` 较大的排在前面**。这意味着对于同一个 Key，较新的事务版本会排在较旧的事务版本之前，极大地方便了 MVCC 读操作（只需找到第一个可见的版本即可）。

## 2. CRUD 操作实现

### 写入与更新 (Put)
`put(key, value, tranc_id)` 实现了数据的插入和原位更新。
1. **随机层级生成**：通过模拟“抛硬币”（50%概率）决定新节点的层级（`level`），层数范围限制在 `[1, max_level]`，兼顾了查找效率和内存消耗。
2. **寻找插入点**：从当前最高层向下遍历，维护一个 `update` 数组，记录每一层插入位置的前驱节点。
3. **处理相同事务更新**：如果找到相同的 `key` 且 `tranc_id` 完全一致，则直接原位更新 `value`。
4. **插入新版本**：如果 `key` 不存在或 `tranc_id` 不同，则将新节点接入链表中。更新时不仅修改 `forward_`，还会维护 `backward_` 指针。

### 查询 (Get)
`get(key, tranc_id)` 结合了跳表的 O(log N) 查找能力和 MVCC 隔离。
1. **多层查找**：从最高层向下，一直查找到第 0 层。
2. **事务可见性判断**：
   - 若 `tranc_id == 0`（未开启事务），直接匹配返回。
   - 若 `tranc_id != 0`，由于相同 Key 的新版本排在前面，沿着第 0 层向后遍历，找到的 **第一个满足 `tranc_id_ <= tranc_id` 的节点** 即为该事务可见的最新版本数据。

### 删除 (Remove)
- **业务删除**：在 LSM-Tree 的语义下，删除通常是通过 `put` 写入一个带有墓碑（Tombstone）标记或空 Value 的记录。
- **物理删除**：跳表自身实现了一个真正的 `remove(key)` 函数（主要为维持数据结构完整性）。从顶层搜索，找到目标节点后，逐层解除 `forward_` 和 `backward_` 指针关系。如果删除了最高层的唯一节点，还会动态降低 `current_level`。

## 3. 迭代器机制 (SkipListIterator)

为了无缝接入 LSM-Tree 的多路归并和范围查询机制，跳表实现了统一的迭代器 `SkipListIterator`（继承自 `BaseIterator`）。
- **底层遍历**：内部封装了 `std::shared_ptr<SkipListNode> current`。
- **单向遍历**：重载了 `operator++`，只需令 `current = current->forward_[0]` 即可在最底层（包含所有元素）实现有序遍历。
- **接口统一**：提供了 `is_valid()`、`get_key()`、`get_value()`、`get_tranc_id()` 等方法，方便外部读取数据及事务版本。

## 4. 范围查询与高级特性 (Range Queries)

跳表的范围查询能力非常强大，除了常规的 `begin()` / `end()`，还针对 LSM 引擎和 Redis 兼容做了针对性优化：

### 前缀匹配查询
- `begin_preffix(prefix)`：利用跳表的特性，快速定位到第一个 `>= prefix` 的节点，避免全表扫描。
- `end_preffix(prefix)`：从跳表高层搜索找到边界，然后顺着底层指针找到第一个不再匹配 `prefix` 的节点，作为前缀范围的终止迭代器。

### 谓词单调查询 (`iters_monotony_predicate`)
这是跳表实现中最精妙的高级查询功能。该方法接受一个谓词函数（如范围、前缀匹配等），并利用跳表的**双向指针（forward 和 backward）**在多层结构中进行二分式收敛：
1. **快速寻找锚点**：从最高层向右遍历，利用谓词的返回值（0表示匹配，<0表示偏右，>0表示偏左）快速找到某一个满足谓词的节点。
2. **向左扩张寻找左边界**：从该节点开始，利用 `backward_` 指针在较高层向左“回退”，快速找到第一个满足谓词的起始迭代器。
3. **向右扩张寻找右边界**：类似地，利用 `forward_` 指针向右快速找到最后一个满足谓词的终止边界。

这种设计使得涉及大范围数据的查询能够**直接在 O(log N) 的时间复杂度下找到确切的左右边界区间**，不需要像普通单向链表那样在底层做线性扫描（O(N)），极大地提升了类似 Redis `ZRANGE` 或 `LRANGE` 等指令的执行效率。

---
**总结**：Tiny-LSM 的跳表实现不仅是一个经典的数据结构教科书版本，还深度融合了持久化存储引擎的需求：包括支持 MVCC 版本排序、内存消耗精确追踪（`size_bytes`）、以及为范围查询专门优化的多层双向指针搜索。