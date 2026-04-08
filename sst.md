# Tiny-LSM SST (Sorted String Table) 模块原理解析

SST (Sorted String Table) 是 LSM-Tree 在磁盘上持久化存储数据的核心结构。在 `Tiny-LSM` 中，当 MemTable (内存跳表) 写满被冻结后，会被异步刷盘（Flush）生成 SST 文件。同时，后台的 Compaction 机制也会不断读取旧的 SST 文件，合并生成新的 SST 文件。

以下是从源码级别对 SST 的文件布局、构建流程、打开与解码、以及单点和范围查询原理的拆解。

## 1. SST 的磁盘文件布局

为了兼顾顺序写入的高效性和随机读取的低延迟，`Tiny-LSM` 的 SST 文件采用了分段式的经典设计。

**整体文件布局**：
```text
------------------------------------------------------------------------------------------------------
|  Block Section (Data)  |  Meta Section (Index)  |  Bloom Filter  |        Trailer (Extra)          |
------------------------------------------------------------------------------------------------------
| Block 0 | ... | Block N| MetaEntry0|...|MetaEntryN | BloomFilterBytes | meta_offset | bloom_offset | min_tx | max_tx |
------------------------------------------------------------------------------------------------------
```

### 核心区域解析：
1. **Block Section (数据区)**：占用最大的区域，由多个排好序的 `Block`（见 `block.md`）连续追加而成。
2. **Meta Section (元数据索引区)**：这是一个非常关键的“目录”。它由多个 `BlockMeta` 序列化而成。每个 `BlockMeta` 对应前面数据区的一个 Block，记录了该 Block 的**起始偏移量 (offset)**、**首键 (first_key)** 和 **尾键 (last_key)**。
3. **Bloom Filter (布隆过滤器)**：保存了当前 SST 文件中所有 Key 的特征指纹。用于在点查时快速过滤掉绝对不存在的 Key，从而节省昂贵的磁盘 I/O。
4. **Trailer (尾部元信息)**：固定大小，放置在文件绝对末尾。主要包含 `Meta Section` 和 `Bloom Filter` 的起始偏移量（均为 4 字节 `uint32_t`），以及该 SST 包含的数据的最大和最小事务 ID（`min_tranc_id`, `max_tranc_id`，各 8 字节 `uint64_t`）。

---

## 2. SST 的构建机制 (`SSTBuilder`)

SST 的构建是一个**纯内存的 Append-only 流式处理过程**，主要由 `SSTBuilder` 类负责：

1. **流式接收数据**：外界（比如 MemTable 刷盘时）通过 `builder.add(key, value, tranc_id)` 不断传入**严格按字典序递增**的键值对。
2. **装填 Block**：`SSTBuilder` 内部维护了一个当前正在写入的 `Block` 对象。数据首先被放进这个 Block 中。同时，提取 Key 的哈希值加入 Bloom Filter。
3. **Block 封块与落盘 (`finish_block`)**：当当前 Block 达到预设的容量阈值（如 4KB）时，触发封块：
   * 将当前 Block 编码（Encode）为字节流，追加到 builder 的 `data` 缓冲区的末尾。
   * 提取该 Block 的起始偏移量、首键和尾键，生成一个 `BlockMeta`，推入 `meta_entries` 索引数组中。
   * 清空当前 Block，准备接收下一批数据。
4. **整体构建落盘 (`build`)**：当所有数据都 add 完后，调用 `build`：
   * 强制 flush 最后一个可能没装满的 Block。
   * 将 `meta_entries` 数组编码追加到缓冲区，记录下此时的偏移量 `meta_block_offset`。
   * 将 Bloom Filter 编码追加到缓冲区，记录下偏移量 `bloom_offset`。
   * 将这两个偏移量以及最大/最小事务 ID 作为 Trailer 追加到绝对末尾。
   * 最后，将整个 `data` 缓冲区的字节流一次性通过 `FileObj` 顺序写入磁盘文件。

---

## 3. SST 的打开与元数据解码 (`SST::open`)

当系统启动或者需要读取 SST 时，SST 文件的加载是**极度轻量级（Lazy Loading）**的。

1. **倒序读取 Trailer**：直接跳到文件的绝对末尾，读取固定的最后几十个字节。从中解出 `meta_block_offset`、`bloom_offset` 以及事务 ID 边界。
2. **按图索骥读取 Meta 与 BloomFilter**：根据刚才拿到的偏移量，去文件中精准读取 Bloom Filter 的字节流和 Meta Section 的字节流，并在内存中反序列化为 `std::shared_ptr<BloomFilter>` 和 `std::vector<BlockMeta>`。
3. **数据块不加载（懒加载）**：**注意，此时并没有读取任何实际的 Data Block。** 数据块只有在真正被查询命中时，才会通过 Block Cache 动态加载。这极大地节省了启动时间和内存占用。

---

## 4. 单点查询的“漏斗过滤”模型 (`SST::get`)

SST 的点查（Point Get）是一个层层递进、尽量避免磁盘 I/O 的漏斗模型，核心逻辑在 `find_block_idx` 中：

1. **第一层过滤：事务 ID 边界过滤**（隐藏在引擎更上层）。如果查询的 `tranc_id` 小于 SST 的 `min_tranc_id_`，说明这整个 SST 里的数据都太新了，直接跳过。
2. **第二层过滤：Bloom Filter 拦截**：将目标 Key 交给内存中的 Bloom Filter。如果 Bloom Filter 说“不存在”，那么它**绝对不存在**，直接返回找不到，完美避免了一次磁盘 I/O。
3. **第三层过滤：BlockMeta 索引二分查找**：如果 Bloom Filter 说“可能存在”，程序会在内存中的 `meta_entries` 数组上进行二分查找。因为每个 `BlockMeta` 都有 `first_key` 和 `last_key`，通过比较可以快速在 `O(log M)` 的时间内锁定目标 Key **必定存在于哪一个 Block 中**。
4. **磁盘 I/O 与 Block Cache**：根据定位到的 `block_idx`，去 `BlockCache` 中要数据。如果 Cache Miss，利用 `BlockMeta` 中的 `offset` 去磁盘文件中精确定位并读取这个 Block 的字节流，解析后放入 Cache。
5. **第四层过滤：Block 内部二分**：最后，将查询任务委托给这个 Block 的原生二分查找接口（详见 `block.md` 的 MVCC 回溯机制），获取最终的 Value。

---

## 5. 迭代器与范围查询 (`SstIterator`)

为了支持全量扫描和范围查询，SST 提供了 `SstIterator`，它的本质是**两级迭代器的嵌套**：

*   **外层状态**：记录当前正在遍历哪个 Block（`block_idx`）。
*   **内层状态**：持有一个 `BlockIterator`，负责在当前的 Block 内部移动。
*   当内层的 `BlockIterator` 走到尽头（Block 尾部）时，外层的 `SstIterator` 会自动触发 I/O（经由 Cache），加载下一个 `block_idx` 的 Block，并重置内层迭代器，对调用者实现无缝跨块扫描。

### 范围查询 (`sst_iters_monotony_predicate`)
如同 SkipList 和 Block，SST 的范围查询也是基于抽象的**单调谓词搜索**实现的，它将寻址开销降到了最低：

1. **块级宏观二分（定位边界 Block）**：
   * 首先在内存的 `meta_entries`（BlockMeta 数组）上执行两次二分查找。
   * 第一次二分利用谓词找到**第一个与查询范围有交集的 Block** 的索引。
   * 第二次二分找到**最后一个与查询范围有交集的 Block** 的索引。
2. **块级微观二分（定位精确 Entry）**：
   * 找到了起始 Block 后，加载该 Block，然后调用该 Block 的 `get_monotony_predicate_iters`（详见 `block.md`）进行微观的二次二分，精准切出起始边界。
   * 同样的方法处理结束 Block，切出精确的结束边界。
3. 通过这两步，SST 可以在不线性扫描大量无用数据块的前提下，直接返回一个严格匹配范围的 `[begin, end)` 嵌套迭代器对。