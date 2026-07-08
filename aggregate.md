# 聚合函数实现 - 修改文件清单

## 一、词法分析器修改

### 文件：src/parser/lex.l
**修改内容：**
1. 添加聚合函数关键字Token定义（在关键字区域）
   - COUNT
   - MAX
   - MIN
   - SUM
2. 在关键字识别规则中添加这些Token的匹配规则

---

## 二、语法分析器修改

### 文件：src/parser/yacc.y
**修改内容：**
1. 在Token声明部分添加聚合函数Token
   ```yacc
   %token COUNT MAX MIN SUM
   ```

2. 修改selector语法规则，支持聚合函数调用
   - 当前selector只支持列名或*
   - 需要扩展支持：聚合函数(列名) as 别名
   - 需要支持：COUNT(*)

3. 添加聚合函数表达式的语法规则
   ```yacc
   agg_func:
       COUNT '(' '*' ')'
     | COUNT '(' col ')'
     | MAX '(' col ')'
     | MIN '(' col ')'
     | SUM '(' col ')'
   ```

4. 修改sel_cols语法规则，支持聚合函数作为选择项
   - 当前：sel_cols -> sel_col | sel_cols ',' sel_col
   - 需要支持：sel_col可以是聚合函数表达式

---

## 三、AST定义修改

### 文件：src/parser/ast.h
**修改内容：**
1. 添加聚合函数类型枚举
   ```cpp
   enum AggType {
       AGG_COUNT,
       AGG_MAX,
       AGG_MIN,
       AGG_SUM
   };
   ```

2. 添加聚合函数表达式AST节点
   ```cpp
   struct AggExpr {
       AggType agg_type;           // 聚合函数类型
       std::string tab_name;       // 表名
       std::string col_name;       // 列名（COUNT(*)时为空）
       bool is_count_star;         // 是否是COUNT(*)
       std::string alias;          // 别名
   };
   ```

3. 修改SelectStmt结构
   - 当前sel_cols是vector<Col>
   - 需要支持混合列和聚合函数
   - 建议：vector<std::variant<Col, AggExpr>> sel_cols
   - 或者：分别存储 vector<Col> normal_cols 和 vector<AggExpr> agg_cols

---

## 四、语义分析修改

### 文件：src/analyze/analyze.cpp
**修改内容：**
1. 添加聚合函数验证函数
   ```cpp
   void check_agg_func(const AggExpr &agg, const TabCol &tab_col);
   ```
   - 检查列是否存在
   - 检查类型兼容性：
     * COUNT：支持int、float、char
     * MAX/MIN：支持int、float、char
     * SUM：只支持int、float（不支持char）
   - 推断聚合函数返回类型：
     * COUNT → int
     * MAX/MIN → 与输入类型相同
     * SUM(int) → int
     * SUM(float) → float

2. 在select_from_check函数中添加聚合函数验证
   - 验证聚合函数参数的合法性
   - 设置聚合函数的返回类型

3. 处理聚合函数的语义约束
   - 聚合函数应该对WHERE过滤后的结果进行计算
   - 如果存在聚合函数，SELECT子句中不能有普通列（除非有GROUP BY）
   - 当前不要求实现GROUP BY，所以简化处理：
     * 要么全是聚合函数
     * 要么全是普通列

---

## 五、执行计划修改

### 文件：src/optimizer/plan.h
**修改内容：**
1. 添加聚合执行计划节点
   ```cpp
   struct AggPlan : public Plan {
       std::shared_ptr<Plan> subplan;           // 子计划（通常是扫描计划）
       std::vector<AggExpr> agg_exprs;          // 聚合函数列表
       std::vector<TabCol> normal_cols;         // 普通列（如果有）
       std::vector<Condition> conds;            // WHERE条件
   };
   ```

2. 可能需要在ProjectionPlan中支持聚合函数
   - 或者为聚合创建单独的计划类型

---

## 六、执行器添加

### 新增文件：src/execution/executor_aggregation.h
**文件内容：**
```cpp
class AggregationExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;   // 前驱执行器
    std::vector<AggExpr> agg_exprs_;           // 聚合函数列表
    std::vector<ColMeta> cols_;                // 输出列元数据
    bool is_end_;                              // 是否已完成
    std::vector<Value> agg_results_;           // 聚合结果
    
public:
    // 实现聚合计算逻辑
    void beginTuple() override;
    void nextTuple() override;
    bool is_end() const override;
    std::unique_ptr<RmRecord> Next() override;
    
private:
    // 具体聚合函数实现
    Value compute_count();
    Value compute_max();
    Value compute_min();
    Value compute_sum();
};
```

**关键实现点：**
1. beginTuple()：遍历所有记录，计算聚合结果
   - 先执行prev_->beginTuple()
   - 遍历所有满足WHERE条件的记录
   - 对每条记录更新聚合值

2. 聚合计算逻辑：
   - COUNT(*): 统计行数
   - COUNT(col): 统计非NULL值的数量（当前系统可能不支持NULL，所以等同于COUNT(*)）
   - MAX(col): 维护最大值
   - MIN(col): 维护最小值
   - SUM(col): 累加求和

3. 类型处理：
   - int类型：直接比较或累加
   - float类型：浮点数比较或累加
   - char类型：字符串比较（MAX/MIN）

4. 输出格式：
   - int类型：直接输出整数
   - float类型：保留6位小数
   - COUNT返回int类型

---

## 七、Portal转换修改

### 文件：src/portal.h
**修改内容：**
1. 在convert_plan_executor函数中添加聚合计划的处理
   ```cpp
   if (auto agg_plan = std::dynamic_pointer_cast<AggPlan>(plan)) {
       // 创建AggregationExecutor
       auto prev_executor = convert_plan_executor(agg_plan->subplan, ...);
       return std::make_unique<AggregationExecutor>(
           std::move(prev_executor), 
           agg_plan->agg_exprs,
           ...
       );
   }
   ```

2. 确保聚合执行器能正确插入到执行器树中
   - 通常在WHERE过滤之后
   - 在Projection之前（如果需要）

---

## 八、查询优化器修改

### 文件：src/optimizer/planner.cpp
**修改内容：**
1. 在生成SELECT执行计划时，检测是否有聚合函数
   ```cpp
   bool has_aggregation(const std::vector<AggExpr> &agg_exprs);
   ```

2. 如果有聚合函数，生成AggPlan
   - 先生成扫描计划（ScanPlan）
   - 应用WHERE条件下推
   - 生成AggPlan包装扫描计划
   - 不需要生成ProjectionPlan（聚合结果直接输出）

3. 如果没有聚合函数，保持原有的投影逻辑

---

## 九、执行管理器修改

### 文件：src/execution/execution_manager.cpp
**修改内容：**
1. 在select_from函数中支持聚合函数的输出
   - 检测SelectStmt中是否有聚合函数
   - 如果有，使用聚合执行器
   - 输出格式适配聚合结果

2. 聚合结果输出格式化
   - int类型：直接输出（不显示小数）
   - float类型：保留6位小数
   - 使用as别名作为列名
   - 示例：
     ```
     | sum_id |
     | 9      |
     
     | sum_val    |
     | 20.000000  |
     ```

---

## 十、类型系统扩展（可选）

### 文件：src/common/common.h
**修改内容：**
1. 可能需要扩展Value结构以支持聚合值的存储
   - 当前Value结构已经足够，可能不需要修改

---

## 实现顺序建议

**建议按以下顺序实现：**

1. **第一步**：修改lex.l和yacc.y，支持聚合函数的语法解析
   - 添加关键字Token
   - 修改语法规则
   - 测试：能正确解析聚合函数SQL

2. **第二步**：修改ast.h，添加AggExpr节点
   - 定义聚合表达式结构
   - 修改SelectStmt以支持聚合函数

3. **第三步**：修改analyze.cpp，添加语义验证
   - 验证聚合函数参数
   - 推断返回类型
   - 检查类型兼容性

4. **第四步**：添加executor_aggregation.h，实现聚合执行器
   - 实现COUNT/MAX/MIN/SUM计算逻辑
   - 处理类型转换和输出格式

5. **第五步**：修改plan.h和planner.cpp，生成聚合执行计划
   - 添加AggPlan节点
   - 生成聚合执行计划逻辑

6. **第六步**：修改portal.h和execution_manager.cpp
   - 支持聚合执行器的创建
   - 支持聚合结果的输出格式化

7. **第七步**：测试验证
   - 按照题目要求的测试点进行测试
   - 验证输出格式符合要求

---

## 测试要点

### 测试点1：SUM聚合
- int类型求和：不显示小数
- float类型求和：保留6位小数
- 别名输出正确

### 测试点2：MAX/MIN聚合
- 返回最大值/最小值
- 支持int、float、char类型
- float类型保留6位小数

### 测试点3：COUNT聚合
- COUNT(*): 统计行数
- COUNT(col): 统计列值数量
- 结合WHERE条件过滤

### 输出格式要求
- 输出到build/{db_name}/output.txt文件
- 表头格式：| alias |
- 值格式：int直接输出，float保留6位小数
