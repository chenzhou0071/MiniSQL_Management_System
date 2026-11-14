#pragma once

#include "../common/Error.h"
#include "../parser/AST.h"
#include <string>
#include <memory>
#include <utility>
#include <vector>

namespace minisql {
namespace optimizer {

// ============================================================
// 查询重写器
// ============================================================
class QueryRewriter {
public:
    // 重写 SELECT 语句
    static std::shared_ptr<parser::SelectStmt> rewrite(parser::SelectStmt* stmt);

    // ========================================================
    // 谓词工具（供计划生成阶段复用）
    // ========================================================
    // 按 AND 拆分合取项：a AND b AND c -> {a, b, c}
    static std::vector<parser::ExprPtr> splitConjuncts(const parser::ExprPtr& expr);

    // 用 AND 重新组合合取项
    static parser::ExprPtr combineConjuncts(const std::vector<parser::ExprPtr>& parts);

    // 收集表达式中所有列引用（表名, 列名），子查询内部不再展开
    static void collectColumnRefs(const parser::ExprPtr& expr,
                                  std::vector<std::pair<std::string, std::string>>& out);

    // 表达式是否包含子查询
    static bool containsSubquery(const parser::ExprPtr& expr);

private:
    // 常量折叠：将编译时常量表达式计算出来
    static Result<parser::ExprPtr> foldConstants(parser::ExprPtr expr);

    // 消除冗余条件
    // 例如: age > 18 OR age = 20 -> age > 18
    static Result<parser::ExprPtr> eliminateRedundantConditions(parser::ExprPtr expr);

    // 谓词下推：将过滤条件下推到更早的阶段执行
    static Result<void> pushDownPredicates(parser::SelectStmt* stmt);

    // 简化布尔表达式
    static Result<parser::ExprPtr> simplifyBooleanExpr(parser::ExprPtr expr);

    // 辅助函数：检查表达式是否恒为真
    static bool isAlwaysTrue(parser::ExprPtr expr);

    // 辅助函数：检查表达式是否恒为假
    static bool isAlwaysFalse(parser::ExprPtr expr);

    // 辅助函数：提取列引用
    static std::string extractColumnName(parser::ExprPtr expr);

    // 去掉列引用中的表限定符：t.a -> a
    static parser::ExprPtr stripTableQualifier(const parser::ExprPtr& expr,
                                               const std::string& qualifier);

    // 子查询的结果列是否支持下推（只投影列引用，或 SELECT *）
    static bool isProjectionPushable(const parser::SelectStmt* subquery);
};

}  // namespace optimizer
}  // namespace minisql
