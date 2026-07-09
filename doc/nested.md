# 块嵌套循环连接算法实现方案

## 一、算法原理与设计思路

### 1.1 算法背景

简单嵌套循环连接（Simple Nested Loop Join）对左表的每一条记录，都需要完整扫描右表一次，导致大量I/O操作。当右表数据量较大时，性能较差。

块嵌套循环连接（Block Nested Loop Join）的优化思路是：
- 将右表分成多个块（Block）
- 每次将一个块完整加载到内存中
- 然后扫描左表的所有记录，与内存中的块进行连接
- 处理完一个块后，再加载下一个块

**性能对比**：
- 简单NLJ：I/O次数 = |左表| × |右表|
- 块NLJ：I/O次数 = |左表| × (|右表| / 块大小)

### 1.2 本项目实现要点

根据题目要求：
1. 测试数据表大小超过内存大小，需要自己定义join_buffer大小
2. 包含非等值连接（如 t1.id < t2.t_id），需要支持任意连接条件
3. 不包含索引，不包含同名字段
4. 测试平台提供2GB内存，buffer大小不应超过此限制

**实现策略**：
- 使用内存缓冲区存储右表的一个块
- 块大小可配置，默认设置为可用内存的合适比例（如1GB）
- 支持所有类型的连接条件（等值、非等值）
- 复用现有的条件评估机制

---

## 二、需要修改/新增的文件

### 2.1 新建文件

#### 文件1: `/home/absinthe/rmdb/src/execution/executor_blocknestedloop_join.h`

**功能**：块嵌套循环连接执行器的实现

**核心设计**：
```cpp
class BlockNestedLoopJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_;      // 左表执行器
    std::unique_ptr<AbstractExecutor> right_;     // 右表执行器
    size_t len_;                                  // 连接后记录长度
    std::vector<ColMeta> cols_;                   // 连接后字段元数据
    std::vector<Condition> fed_conds_;            // 连接条件
    bool isend;                                   // 是否结束
    
    // 块相关成员
    std::vector<std::unique_ptr<RmRecord>> block_buffer_;  // 块缓冲区
    size_t block_size_;                                     // 块大小（记录数）
    size_t current_block_index_;                            // 当前块内索引
    bool block_loaded_;                                     // 当前块是否已加载
    size_t right_tuple_count_;                              // 右表总记录数（用于预估）
    
public:
    // 构造函数
    BlockNestedLoopJoinExecutor(
        std::unique_ptr<AbstractExecutor> left,
        std::unique_ptr<AbstractExecutor> right,
        std::vector<Condition> conds,
        size_t block_size = 100000  // 默认块大小
    );
    
    // 标准执行器接口
    void beginTuple() override;
    void nextTuple() override;
    std::unique_ptr<RmRecord> Next() override;
    bool is_end() const override;
    const std::vector<ColMeta> &cols() const override;
    size_t tupleLen() const override;
    Rid &rid() override;
    
private:
    // 块管理方法
    bool load_next_block();           // 加载下一个块，返回是否成功
    void clear_block_buffer();        // 清空块缓冲区
    bool find_next_match();           // 查找下一个匹配的记录对
};
```

**关键实现细节**：

1. **块加载逻辑** (`load_next_block`):
   ```cpp
   bool load_next_block() {
       clear_block_buffer();
       
       // 加载右表的下一批记录到内存
       for (size_t i = 0; i < block_size_ && !right_->is_end(); i++) {
           auto rec = right_->Next();
           block_buffer_.push_back(std::move(rec));
           right_->nextTuple();
       }
       
       return !block_buffer_.empty();
   }
   ```

2. **连接主循环** (`beginTuple`):
   ```cpp
   void beginTuple() {
       left_->beginTuple();
       if (left_->is_end()) {
           isend = true;
           return;
       }
       
       right_->beginTuple();
       block_loaded_ = load_next_block();
       current_block_index_ = 0;
       
       find_next_match();
   }
   ```

3. **查找匹配** (`find_next_match`):
   ```cpp
   bool find_next_match() {
       while (true) {
           // 当前块已处理完
           if (current_block_index_ >= block_buffer_.size()) {
               // 尝试加载下一个块
               right_->beginTuple();  // 重置右表扫描
               // 跳过已处理的块
               // ... (需要维护偏移量)
               
               block_loaded_ = load_next_block();
               if (!block_loaded_) {
                   // 右表所有块处理完，处理左表下一条
                   left_->nextTuple();
                   if (left_->is_end()) {
                       isend = true;
                       return false;
                   }
                   // 重新开始扫描右表
                   right_->beginTuple();
                   block_loaded_ = load_next_block();
                   current_block_index_ = 0;
               } else {
                   current_block_index_ = 0;
                   continue;
               }
           }
           
           // 检查当前左表记录与块内当前记录是否满足条件
           auto left_rec = left_->Next();
           auto right_rec = block_buffer_[current_block_index_].get();
           
           auto rec = std::make_unique<RmRecord>(len_);
           memcpy(rec->data, left_rec->data, left_->tupleLen());
           memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
           
           if (eval_conds(cols_, fed_conds_, rec.get())) {
               return true;
           }
           
           current_block_index_++;
       }
   }
   ```

**重要注意事项**：
- 右表的多次扫描需要从头开始，注意重置右表执行器
- 块加载时需要跳过已处理的记录，维护扫描位置
- 内存管理：块缓冲区大小 = 块大小 × 记录大小，需确保不超内存限制

---

### 2.2 修改文件

#### 文件2: `/home/absinthe/rmdb/src/optimizer/plan.h`

**修改位置**：第22-47行，PlanTag枚举定义

**修改内容**：添加新的Plan标签

```cpp
typedef enum PlanTag{
    T_Invalid = 1,
    T_Help,
    T_ShowTable,
    T_DescTable,
    T_CreateTable,
    T_DropTable,
    T_CreateIndex,
    T_DropIndex,
    T_ShowIndex,
    T_Insert,
    T_Update,
    T_Delete,
    T_select,
    T_Transaction_begin,
    T_Transaction_commit,
    T_Transaction_abort,
    T_Transaction_rollback,
    T_SeqScan,
    T_IndexScan,
    T_NestLoop,
    T_BlockNestLoop,    // 新增：块嵌套循环连接
    T_Sort,
    T_Limit,
    T_Projection,
    T_Aggregation
} PlanTag;
```

**修改原因**：标识块嵌套循环连接计划类型，用于后续执行器创建

---

#### 文件3: `/home/absinthe/rmdb/src/portal.h`

**修改位置1**：第18行，添加头文件引用

```cpp
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_blocknestedloop_join.h"  // 新增
#include "execution/executor_projection.h"
```

**修改位置2**：第183-189行，`convert_plan_executor`函数中JoinPlan处理逻辑

**当前代码**：
```cpp
} else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
    std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
    std::unique_ptr<AbstractExecutor> right = convert_plan_executor(x->right_, context);
    std::unique_ptr<AbstractExecutor> join = std::make_unique<NestedLoopJoinExecutor>(
                        std::move(left), 
                        std::move(right), std::move(x->conds_));
    return join;
```

**修改后代码**：
```cpp
} else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
    std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
    std::unique_ptr<AbstractExecutor> right = convert_plan_executor(x->right_, context);
    
    std::unique_ptr<AbstractExecutor> join;
    if (x->tag == T_BlockNestLoop) {
        // 使用块嵌套循环连接
        join = std::make_unique<BlockNestedLoopJoinExecutor>(
                    std::move(left), 
                    std::move(right), 
                    std::move(x->conds_),
                    100000);  // 块大小可配置
    } else {
        // 使用简单嵌套循环连接
        join = std::make_unique<NestedLoopJoinExecutor>(
                    std::move(left), 
                    std::move(right), 
                    std::move(x->conds_));
    }
    return join;
```

**修改原因**：根据Plan标签选择创建对应的连接执行器

---

#### 文件4（可选）: `/home/absinthe/rmdb/src/optimizer/planner.cpp`

**修改位置**：`make_one_rel()`函数中连接计划创建部分

**修改目的**：根据表大小选择合适的连接算法（可选优化）

**示例修改**：
```cpp
// 原代码创建连接计划
auto join_plan = std::make_shared<JoinPlan>(
    T_NestLoop, 
    left_plan, 
    right_plan, 
    join_conds
);

// 修改为根据表大小选择
PlanTag join_tag = T_NestLoop;
if (right_table_size > MEMORY_THRESHOLD) {
    join_tag = T_BlockNestLoop;
}

auto join_plan = std::make_shared<JoinPlan>(
    join_tag, 
    left_plan, 
    right_plan, 
    join_conds
);
```

---

## 三、具体实现步骤

### 步骤1：创建执行器头文件

**文件**：`src/execution/executor_blocknestedloop_join.h`

**实现要点**：
1. 继承`AbstractExecutor`，保持接口一致性
2. 实现块缓冲区管理
3. 实现右表的多次扫描逻辑（关键难点）
4. 实现条件评估，复用基类的`eval_conds`方法

**右表多次扫描的实现挑战**：
- 问题：右表执行器一旦扫描到末尾，无法直接重置到开头
- 解决方案：
  - 方案A：在`beginTuple`时保存右表的初始状态，每次重置时恢复
  - 方案B：重新创建右表执行器（推荐，更简单）
  - 方案C：在块加载时记录扫描位置，下次从该位置继续

**推荐方案B的实现**：
```cpp
// 需要保存右表的创建信息，以便重新创建执行器
std::shared_ptr<Plan> right_plan_;  // 右表计划
Context* context_;                   // 上下文
SmManager* sm_manager_;              // 系统管理器

// 重新创建右表执行器
void reset_right_executor() {
    // 重新创建右表扫描执行器
    right_ = create_executor_from_plan(right_plan_, context_);
    right_->beginTuple();
}
```

---

### 步骤2：修改Plan标签

**文件**：`src/optimizer/plan.h`

**操作**：在`PlanTag`枚举中添加`T_BlockNestLoop`

**注意**：确保枚举值唯一，不要与现有标签冲突

---

### 步骤3：修改Portal执行器创建逻辑

**文件**：`src/portal.h`

**操作**：
1. 添加头文件引用
2. 修改`convert_plan_executor`函数，根据Plan标签选择执行器类型

---

### 步骤4：测试验证

**测试场景**：
1. 等值连接：`select * from t1, t2 where t1.id = t2.t_id`
2. 非等值连接：`select * from t1, t2 where t1.id < t2.t_id and t2.t_id < 1000`
3. 大表连接：测试数据超过内存大小的情况

**验证要点**：
- 结果正确性：与简单NLJ的结果一致
- 性能提升：大表连接的执行时间应显著减少
- 内存使用：块缓冲区大小不超过限制

---

## 四、关键代码实现细节

### 4.1 块大小配置

**推荐值**：
- 可用内存：2GB
- 块大小建议：1GB（留一半给其他开销）
- 记录数估算：块大小 / 平均记录大小

**配置方式**：
```cpp
// 方法1：硬编码（简单）
const size_t DEFAULT_BLOCK_SIZE = 100000;  // 10万条记录

// 方法2：运行时计算（推荐）
size_t calculate_block_size(size_t record_size) {
    const size_t MEMORY_LIMIT = 1024 * 1024 * 1024;  // 1GB
    return MEMORY_LIMIT / record_size;
}
```

### 4.2 右表重置的正确实现

**关键问题**：如何实现右表的多次完整扫描？

**解决方案**：在`BlockNestedLoopJoinExecutor`中保存右表计划的引用

```cpp
class BlockNestedLoopJoinExecutor : public AbstractExecutor {
private:
    std::shared_ptr<Plan> right_plan_;  // 保存右表计划
    Portal* portal_;                    // Portal引用，用于重新创建执行器
    
    // 重新创建并初始化右表执行器
    void recreate_right_executor() {
        right_ = portal_->convert_plan_executor(right_plan_, context_);
        right_->beginTuple();
    }
};
```

**修改Portal接口**：
需要在`convert_plan_executor`返回执行器的同时，保留计划的引用。

### 4.3 处理已扫描块的问题

**问题描述**：加载第N个块时，需要跳过前N-1个块

**解决方案**：维护块计数器

```cpp
size_t processed_block_count_;  // 已处理的块数量

bool load_next_block() {
    clear_block_buffer();
    
    // 跳过已处理的块
    if (processed_block_count_ > 0) {
        size_t skip_count = processed_block_count_ * block_size_;
        for (size_t i = 0; i < skip_count && !right_->is_end(); i++) {
            right_->nextTuple();
        }
    }
    
    // 加载当前块
    for (size_t i = 0; i < block_size_ && !right_->is_end(); i++) {
        auto rec = right_->Next();
        block_buffer_.push_back(std::move(rec));
        right_->nextTuple();
    }
    
    processed_block_count_++;
    return !block_buffer_.empty();
}
```

**优化建议**：这种跳过方式效率不高，建议直接重新创建执行器并顺序扫描到指定位置。

---

## 五、完整实现伪代码

```cpp
void BlockNestedLoopJoinExecutor::beginTuple() {
    left_->beginTuple();
    if (left_->is_end()) {
        isend = true;
        return;
    }
    
    // 初始化右表扫描
    right_->beginTuple();
    processed_block_count_ = 0;
    load_next_block();
    current_block_index_ = 0;
    
    find_next_match();
}

void BlockNestedLoopJoinExecutor::nextTuple() {
    current_block_index_++;
    find_next_match();
}

bool BlockNestedLoopJoinExecutor::find_next_match() {
    while (true) {
        // 检查当前块是否已扫描完
        if (current_block_index_ >= block_buffer_.size()) {
            // 尝试加载下一个块
            bool has_more = load_next_block();
            
            if (!has_more) {
                // 右表所有块扫描完，处理左表下一条
                left_->nextTuple();
                if (left_->is_end()) {
                    isend = true;
                    return false;
                }
                
                // 重置右表扫描
                recreate_right_executor();
                processed_block_count_ = 0;
                load_next_block();
            }
            
            current_block_index_ = 0;
            continue;
        }
        
        // 检查连接条件
        auto left_rec = left_->Next();
        auto right_rec = block_buffer_[current_block_index_].get();
        
        auto join_rec = std::make_unique<RmRecord>(len_);
        memcpy(join_rec->data, left_rec->data, left_->tupleLen());
        memcpy(join_rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        
        if (eval_conds(cols_, fed_conds_, join_rec.get())) {
            return true;  // 找到匹配
        }
        
        current_block_index_++;
    }
}

std::unique_ptr<RmRecord> BlockNestedLoopJoinExecutor::Next() {
    auto left_rec = left_->Next();
    auto right_rec = block_buffer_[current_block_index_].get();
    
    auto rec = std::make_unique<RmRecord>(len_);
    memcpy(rec->data, left_rec->data, left_->tupleLen());
    memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
    
    return rec;
}
```

---

## 六、性能优化建议

### 6.1 块大小调优
- 过小：无法充分利用内存，I/O次数多
- 过大：可能超出内存限制，引发OOM
- 建议：根据实际表大小和可用内存动态调整

### 6.2 选择左表和右表
- 理论上，应该将较小的表作为右表（加载到内存）
- 实际上，优化器可以根据表统计信息自动选择

### 6.3 预取优化
- 在处理当前块时，可以异步预取下一个块
- 需要额外的缓冲区和线程管理

---

## 七、常见问题与解决方案

### Q1: 右表如何多次扫描？
**A**: 保存右表的计划引用，每次重新创建执行器

### Q2: 如何确定块大小？
**A**: 根据可用内存和记录大小计算，建议设置为1GB左右

### Q3: 非等值连接如何处理？
**A**: 复用现有的`eval_conds`方法，无需特殊处理

### Q4: 如何处理大结果集？
**A**: 使用生成器模式（迭代器），不一次性加载所有结果

### Q5: 如何验证正确性？
**A**: 与简单NLJ的结果对比，确保一致

---

## 八、测试计划

### 8.1 单元测试
- 测试块加载功能
- 测试右表重置功能
- 测试条件评估功能

### 8.2 集成测试
- 小表连接（验证正确性）
- 大表连接（验证性能）
- 非等值连接（验证通用性）

### 8.3 性能测试
- 对比Simple NLJ和Block NLJ的执行时间
- 测试不同块大小的影响
- 测试内存使用情况

---

## 九、开发顺序建议

1. **第一步**：修改`plan.h`，添加`T_BlockNestLoop`标签
2. **第二步**：实现`executor_blocknestedloop_join.h`核心逻辑
3. **第三步**：修改`portal.h`，添加执行器创建逻辑
4. **第四步**：编译测试，修复编译错误
5. **第五步**：功能测试，验证结果正确性
6. **第六步**：性能测试，对比性能提升
7. **第七步**：（可选）优化器自动选择连接算法

---

## 十、注意事项

1. **内存管理**：确保块缓冲区不超过内存限制
2. **异常处理**：处理内存分配失败的情况
3. **边界条件**：处理空表、单条记录等特殊情况
4. **代码风格**：遵循项目现有的代码规范
5. **版权声明**：在文件头部添加版权声明
6. **注释**：添加必要的注释，说明关键逻辑

---

## 总结

本方案详细描述了块嵌套循环连接算法的实现方法，包括：
- 算道和原理说明
- 文件修改列表和具体修改方法
- 关键实现细节和代码示例
- 性能优化建议和测试计划

按照本方案实施，可以正确实现块嵌套循环连接算法，显著提升大表连接的性能。
