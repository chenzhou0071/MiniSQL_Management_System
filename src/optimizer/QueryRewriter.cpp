#include "QueryRewriter.h"
#include "../common/Logger.h"
#include "../executor/ExpressionEvaluator.h"
#include <algorithm>

using namespace minisql;
using namespace optimizer;

std::shared_ptr<parser::SelectStmt> QueryRewriter::rewrite(parser::SelectStmt* stmt) {
    if (!stmt) {
        return nullptr;
    }

    // 复制语句以避免修改原语句
    auto rewritten = std::make_shared<parser::SelectStmt>(*stmt);

    // 1. 谓词下推
    LOG_DEBUG("QueryRewriter", "Step 1: pushDownPredicates");
    auto pushResult = pushDownPredicates(rewritten.get());
    if (pushResult.isError()) {
        return nullptr;
    }

    // 2. 消除冗余条件
    LOG_DEBUG("QueryRewriter", "Step 2: eliminateRedundantConditions");
    if (rewritten->whereClause) {
        auto elimResult = eliminateRedundantConditions(rewritten->whereClause);
        if (elimResult.isSuccess()) {
            rewritten->whereClause = *elimResult.getValue();
        }
    }

    // 3. 常量折叠
    LOG_DEBUG("QueryRewriter", "Step 3: foldConstants");
    if (rewritten->whereClause) {
        auto foldResult = foldConstants(rewritten->whereClause);
        if (foldResult.isSuccess()) {
            rewritten->whereClause = *foldResult.getValue();
        }
    }

    // 4. 简化布尔表达式
    LOG_DEBUG("QueryRewriter", "Step 4: simplifyBooleanExpr");
    if (rewritten->whereClause) {
        auto simpResult = simplifyBooleanExpr(rewritten->whereClause);
        if (simpResult.isSuccess()) {
            rewritten->whereClause = *simpResult.getValue();
        }
    }

    LOG_INFO("QueryRewriter", "Query rewritten successfully");
    return rewritten;
}

Result<parser::ExprPtr> QueryRewriter::foldConstants(parser::ExprPtr expr) {
    if (!expr) {
        return Result<parser::ExprPtr>(expr);
    }

    // 处理 IN_SUBQUERY_EXPR 和 EXISTS_EXPR - 直接返回
    if (expr->getType() == parser::ASTNodeType::IN_SUBQUERY_EXPR ||
        expr->getType() == parser::ASTNodeType::EXISTS_EXPR) {
        return Result<parser::ExprPtr>(expr);
    }

    // 处理二元表达式
    if (expr->getType() == parser::ASTNodeType::BINARY_EXPR) {
        auto binExpr = std::dynamic_pointer_cast<parser::BinaryExpr>(expr);
        if (binExpr) {
            // 递归处理左右操作数
            Result<parser::ExprPtr> leftResult = foldConstants(binExpr->getLeft());
            Result<parser::ExprPtr> rightResult = foldConstants(binExpr->getRight());

            if (leftResult.isSuccess() && rightResult.isSuccess()) {
                auto left = *leftResult.getValue();
                auto right = *rightResult.getValue();

                // 检查是否都是字面量（常量）
                if (left && left->getType() == parser::ASTNodeType::LITERAL_EXPR &&
                    right && right->getType() == parser::ASTNodeType::LITERAL_EXPR) {

                    auto leftLit = std::dynamic_pointer_cast<parser::LiteralExpr>(left);
                    auto rightLit = std::dynamic_pointer_cast<parser::LiteralExpr>(right);

                    if (leftLit && rightLit) {
                        std::string op = binExpr->getOp();
                        std::transform(op.begin(), op.end(), op.begin(), ::toupper);

                        // 尝试进行常量计算
                        try {
                            if (op == "+" || op == "-" || op == "*" || op == "/") {
                                // 数值运算
                                auto leftVal = leftLit->getValue();
                                auto rightVal = rightLit->getValue();

                                if (leftVal.isNumeric() && rightVal.isNumeric()) {
                                    Value result;
                                    if (op == "+") {
                                        result = Value(leftVal.getInt() + rightVal.getInt());
                                    } else if (op == "-") {
                                        result = Value(leftVal.getInt() - rightVal.getInt());
                                    } else if (op == "*") {
                                        result = Value(leftVal.getInt() * rightVal.getInt());
                                    } else if (op == "/") {
                                        if (rightVal.getInt() != 0) {
                                            result = Value(leftVal.getInt() / rightVal.getInt());
                                        }
                                    }
                                    return Result<parser::ExprPtr>(
                                        std::make_shared<parser::LiteralExpr>(result));
                                }
                            }
                        } catch (...) {
                            // 计算失败，返回原表达式
                        }
                    }
                }

                // 返回带有折叠后操作数的新表达式
                auto newExpr = std::make_shared<parser::BinaryExpr>(
                    left, binExpr->getOp(), right);
                return Result<parser::ExprPtr>(newExpr);
            }
        }
    }

    // 处理一元表达式
    if (expr->getType() == parser::ASTNodeType::UNARY_EXPR) {
        auto unaryExpr = std::dynamic_pointer_cast<parser::UnaryExpr>(expr);
        if (unaryExpr) {
            auto operandResult = foldConstants(unaryExpr->getOperand());
            if (operandResult.isSuccess() && operandResult.getValue()) {
                auto newExpr = std::make_shared<parser::UnaryExpr>(
                    unaryExpr->getOp(), *operandResult.getValue());
                return Result<parser::ExprPtr>(newExpr);
            }
        }
    }

    return Result<parser::ExprPtr>(expr);
}

Result<parser::ExprPtr> QueryRewriter::eliminateRedundantConditions(parser::ExprPtr expr) {
    if (!expr) {
        return Result<parser::ExprPtr>(expr);
    }

    // 处理 IN_SUBQUERY_EXPR 和 EXISTS_EXPR - 直接返回
    if (expr->getType() == parser::ASTNodeType::IN_SUBQUERY_EXPR ||
        expr->getType() == parser::ASTNodeType::EXISTS_EXPR) {
        return Result<parser::ExprPtr>(expr);
    }

    if (expr->getType() == parser::ASTNodeType::BINARY_EXPR) {
        auto binExpr = std::dynamic_pointer_cast<parser::BinaryExpr>(expr);
        if (binExpr) {
            std::string op = binExpr->getOp();
            std::transform(op.begin(), op.end(), op.begin(), ::toupper);

            if (op == "AND") {
                // 递归处理 AND 的左右分支
                auto leftResult = eliminateRedundantConditions(binExpr->getLeft());
                auto rightResult = eliminateRedundantConditions(binExpr->getRight());

                if (leftResult.isSuccess() && rightResult.isSuccess()) {
                    auto left = *leftResult.getValue();
                    auto right = *rightResult.getValue();

                    // 如果任一边恒为 false，整个表达式为 false
                    if (isAlwaysFalse(left) || isAlwaysFalse(right)) {
                        return Result<parser::ExprPtr>(
                            std::make_shared<parser::LiteralExpr>(Value(false)));
                    }

                    // 如果一边恒为 true，直接返回另一边
                    if (isAlwaysTrue(left)) {
                        return Result<parser::ExprPtr>(right);
                    }
                    if (isAlwaysTrue(right)) {
                        return Result<parser::ExprPtr>(left);
                    }

                    // 如果两边相同，只保留一个
                    if (left && right && left->toString() == right->toString()) {
                        return Result<parser::ExprPtr>(left);
                    }

                    return Result<parser::ExprPtr>(
                        std::make_shared<parser::BinaryExpr>(left, binExpr->getOp(), right));
                }
            } else if (op == "OR") {
                auto leftResult = eliminateRedundantConditions(binExpr->getLeft());
                auto rightResult = eliminateRedundantConditions(binExpr->getRight());

                if (leftResult.isSuccess() && rightResult.isSuccess()) {
                    auto left = *leftResult.getValue();
                    auto right = *rightResult.getValue();

                    // 如果任一边恒为 true，整个表达式为 true
                    if (isAlwaysTrue(left) || isAlwaysTrue(right)) {
                        return Result<parser::ExprPtr>(
                            std::make_shared<parser::LiteralExpr>(Value(true)));
                    }

                    // 如果一边恒为 false，直接返回另一边
                    if (isAlwaysFalse(left)) {
                        return Result<parser::ExprPtr>(right);
                    }
                    if (isAlwaysFalse(right)) {
                        return Result<parser::ExprPtr>(left);
                    }

                    // 简化：age > 18 OR age = 20 -> age >= 18
                    // 这里需要更复杂的逻辑，简单版本只处理完全相同的情况
                    if (left && right && left->toString() == right->toString()) {
                        return Result<parser::ExprPtr>(left);
                    }

                    return Result<parser::ExprPtr>(
                        std::make_shared<parser::BinaryExpr>(left, binExpr->getOp(), right));
                }
            }
        }
    }

    return Result<parser::ExprPtr>(expr);
}

Result<void> QueryRewriter::pushDownPredicates(parser::SelectStmt* stmt) {
    if (!stmt) {
        return Result<void>();
    }

    // 先递归处理更内层的子查询，多层嵌套时可以一路推到底
    if (stmt->fromTable && stmt->fromTable->subquery) {
        auto innerResult = pushDownPredicates(stmt->fromTable->subquery.get());
        if (innerResult.isError()) {
            return innerResult;
        }
    }

    // 只有 FROM 子查询才有可下推的目标
    if (!stmt->whereClause || !stmt->fromTable || !stmt->fromTable->subquery) {
        return Result<void>();
    }

    const std::string alias = stmt->fromTable->alias;
    auto* subquery = stmt->fromTable->subquery.get();

    // 无法安全下推的场景一律跳过：
    // - 无别名：无法判断谓词中的列是否属于该子查询
    // - 含 GROUP BY / HAVING / LIMIT / OFFSET / 聚合：过滤位置会改变结果
    // - 子查询自身有 JOIN：列名可能歧义
    // - 投影是表达式或常量：外层列名在子查询内部并不存在
    if (alias.empty() || !subquery ||
        !subquery->groupBy.empty() || subquery->havingClause ||
        subquery->limit >= 0 || subquery->offset > 0 ||
        !subquery->joins.empty() || !isProjectionPushable(subquery)) {
        return Result<void>();
    }

    // 判断子查询的投影里是否包含某个列（SELECT * 视为全部包含）
    auto projectionHasColumn = [subquery](const std::string& column) {
        if (subquery->selectItems.empty()) {
            return true;
        }
        for (const auto& item : subquery->selectItems) {
            auto colRef = std::dynamic_pointer_cast<parser::ColumnRef>(item);
            if (colRef && (colRef->getColumn() == "*" || colRef->getColumn() == column)) {
                return true;
            }
        }
        return false;
    };

    std::vector<parser::ExprPtr> conjuncts = splitConjuncts(stmt->whereClause);
    std::vector<parser::ExprPtr> remain;

    for (const auto& cond : conjuncts) {
        std::vector<std::pair<std::string, std::string>> columns;
        collectColumnRefs(cond, columns);

        // 只下推"列引用全部由该子查询别名限定"且不含子查询的谓词
        bool pushable = !columns.empty() && !containsSubquery(cond);
        for (const auto& col : columns) {
            if (col.first != alias || !projectionHasColumn(col.second)) {
                pushable = false;
                break;
            }
        }

        if (!pushable) {
            remain.push_back(cond);
            continue;
        }

        // 子查询内部看不到外层别名，去掉限定符后再落入子查询的 WHERE
        parser::ExprPtr innerCond = stripTableQualifier(cond, alias);
        subquery->whereClause = subquery->whereClause
            ? std::make_shared<parser::BinaryExpr>(subquery->whereClause, "AND", innerCond)
            : innerCond;

        LOG_DEBUG("QueryRewriter", "Pushed predicate into subquery: " + innerCond->toString());
    }

    stmt->whereClause = combineConjuncts(remain);
    return Result<void>();
}

// ============================================================
// 谓词工具：拆分/组合合取项、收集列引用、判断子查询
// ============================================================

std::vector<parser::ExprPtr> QueryRewriter::splitConjuncts(const parser::ExprPtr& expr) {
    std::vector<parser::ExprPtr> parts;
    if (!expr) {
        return parts;
    }

    if (expr->getType() == parser::ASTNodeType::BINARY_EXPR) {
        auto binExpr = std::dynamic_pointer_cast<parser::BinaryExpr>(expr);
        if (binExpr) {
            std::string op = binExpr->getOp();
            std::transform(op.begin(), op.end(), op.begin(), ::toupper);
            if (op == "AND") {
                auto leftParts = splitConjuncts(binExpr->getLeft());
                auto rightParts = splitConjuncts(binExpr->getRight());
                parts.insert(parts.end(), leftParts.begin(), leftParts.end());
                parts.insert(parts.end(), rightParts.begin(), rightParts.end());
                return parts;
            }
        }
    }

    parts.push_back(expr);
    return parts;
}

parser::ExprPtr QueryRewriter::combineConjuncts(const std::vector<parser::ExprPtr>& parts) {
    parser::ExprPtr result;
    for (const auto& part : parts) {
        if (!part) {
            continue;
        }
        result = result ? std::make_shared<parser::BinaryExpr>(result, "AND", part) : part;
    }
    return result;
}

void QueryRewriter::collectColumnRefs(
    const parser::ExprPtr& expr,
    std::vector<std::pair<std::string, std::string>>& out) {
    if (!expr) {
        return;
    }

    switch (expr->getType()) {
        case parser::ASTNodeType::COLUMN_REF: {
            auto colRef = std::static_pointer_cast<parser::ColumnRef>(expr);
            out.emplace_back(colRef->getTable(), colRef->getColumn());
            break;
        }
        case parser::ASTNodeType::BINARY_EXPR: {
            auto binExpr = std::static_pointer_cast<parser::BinaryExpr>(expr);
            collectColumnRefs(binExpr->getLeft(), out);
            collectColumnRefs(binExpr->getRight(), out);
            break;
        }
        case parser::ASTNodeType::UNARY_EXPR: {
            auto unaryExpr = std::static_pointer_cast<parser::UnaryExpr>(expr);
            collectColumnRefs(unaryExpr->getOperand(), out);
            break;
        }
        case parser::ASTNodeType::FUNCTION_CALL: {
            auto funcExpr = std::static_pointer_cast<parser::FunctionCallExpr>(expr);
            for (const auto& arg : funcExpr->getArgs()) {
                collectColumnRefs(arg, out);
            }
            break;
        }
        case parser::ASTNodeType::IN_SUBQUERY_EXPR: {
            auto inExpr = std::static_pointer_cast<parser::InSubqueryExpr>(expr);
            collectColumnRefs(inExpr->getLeft(), out);
            break;
        }
        default:
            break;
    }
}

bool QueryRewriter::containsSubquery(const parser::ExprPtr& expr) {
    if (!expr) {
        return false;
    }

    switch (expr->getType()) {
        case parser::ASTNodeType::SUBQUERY_EXPR:
        case parser::ASTNodeType::IN_SUBQUERY_EXPR:
        case parser::ASTNodeType::EXISTS_EXPR:
            return true;
        case parser::ASTNodeType::BINARY_EXPR: {
            auto binExpr = std::static_pointer_cast<parser::BinaryExpr>(expr);
            return containsSubquery(binExpr->getLeft()) || containsSubquery(binExpr->getRight());
        }
        case parser::ASTNodeType::UNARY_EXPR: {
            auto unaryExpr = std::static_pointer_cast<parser::UnaryExpr>(expr);
            return containsSubquery(unaryExpr->getOperand());
        }
        case parser::ASTNodeType::FUNCTION_CALL: {
            auto funcExpr = std::static_pointer_cast<parser::FunctionCallExpr>(expr);
            for (const auto& arg : funcExpr->getArgs()) {
                if (containsSubquery(arg)) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

parser::ExprPtr QueryRewriter::stripTableQualifier(const parser::ExprPtr& expr,
                                                   const std::string& qualifier) {
    if (!expr) {
        return expr;
    }

    switch (expr->getType()) {
        case parser::ASTNodeType::COLUMN_REF: {
            auto colRef = std::static_pointer_cast<parser::ColumnRef>(expr);
            if (colRef->getTable() == qualifier) {
                return std::make_shared<parser::ColumnRef>("", colRef->getColumn());
            }
            return expr;
        }
        case parser::ASTNodeType::BINARY_EXPR: {
            auto binExpr = std::static_pointer_cast<parser::BinaryExpr>(expr);
            return std::make_shared<parser::BinaryExpr>(
                stripTableQualifier(binExpr->getLeft(), qualifier),
                binExpr->getOp(),
                stripTableQualifier(binExpr->getRight(), qualifier));
        }
        case parser::ASTNodeType::UNARY_EXPR: {
            auto unaryExpr = std::static_pointer_cast<parser::UnaryExpr>(expr);
            return std::make_shared<parser::UnaryExpr>(
                unaryExpr->getOp(),
                stripTableQualifier(unaryExpr->getOperand(), qualifier));
        }
        case parser::ASTNodeType::FUNCTION_CALL: {
            auto funcExpr = std::static_pointer_cast<parser::FunctionCallExpr>(expr);
            std::vector<parser::ExprPtr> args;
            args.reserve(funcExpr->getArgs().size());
            for (const auto& arg : funcExpr->getArgs()) {
                args.push_back(stripTableQualifier(arg, qualifier));
            }
            return std::make_shared<parser::FunctionCallExpr>(funcExpr->getFuncName(), args);
        }
        default:
            return expr;
    }
}

bool QueryRewriter::isProjectionPushable(const parser::SelectStmt* subquery) {
    if (!subquery) {
        return false;
    }

    // SELECT * 或者没有投影列表，视为投影所有列
    if (subquery->selectItems.empty()) {
        return true;
    }

    for (const auto& item : subquery->selectItems) {
        if (!item || item->getType() != parser::ASTNodeType::COLUMN_REF) {
            return false;  // 投影是表达式/常量/函数：外层列名在子查询内部不存在
        }
        auto colRef = std::dynamic_pointer_cast<parser::ColumnRef>(item);
        if (!colRef) {
            return false;
        }
        if (colRef->getColumn() == "*") {
            return true;
        }
    }

    return true;
}

Result<parser::ExprPtr> QueryRewriter::simplifyBooleanExpr(parser::ExprPtr expr) {
    if (!expr) {
        return Result<parser::ExprPtr>(expr);
    }

    // 处理 IN_SUBQUERY_EXPR 和 EXISTS_EXPR - 直接返回，不递归处理
    if (expr->getType() == parser::ASTNodeType::IN_SUBQUERY_EXPR ||
        expr->getType() == parser::ASTNodeType::EXISTS_EXPR) {
        return Result<parser::ExprPtr>(expr);
    }

    // 处理二元表达式
    if (expr->getType() == parser::ASTNodeType::BINARY_EXPR) {
        auto binExpr = std::dynamic_pointer_cast<parser::BinaryExpr>(expr);
        if (binExpr) {
            std::string op = binExpr->getOp();
            std::transform(op.begin(), op.end(), op.begin(), ::toupper);

            if (op == "AND" || op == "OR") {
                auto leftResult = simplifyBooleanExpr(binExpr->getLeft());
                auto rightResult = simplifyBooleanExpr(binExpr->getRight());

                if (leftResult.isSuccess() && rightResult.isSuccess()) {
                    auto left = *leftResult.getValue();
                    auto right = *rightResult.getValue();

                    return Result<parser::ExprPtr>(
                        std::make_shared<parser::BinaryExpr>(left, binExpr->getOp(), right));
                }
            }
        }
    }

    return Result<parser::ExprPtr>(expr);
}

bool QueryRewriter::isAlwaysTrue(parser::ExprPtr expr) {
    if (!expr) return false;

    if (expr->getType() == parser::ASTNodeType::LITERAL_EXPR) {
        auto lit = std::dynamic_pointer_cast<parser::LiteralExpr>(expr);
        if (lit) {
            return lit->getValue().getBool() == true;
        }
    }

    return false;
}

bool QueryRewriter::isAlwaysFalse(parser::ExprPtr expr) {
    if (!expr) return false;

    if (expr->getType() == parser::ASTNodeType::LITERAL_EXPR) {
        auto lit = std::dynamic_pointer_cast<parser::LiteralExpr>(expr);
        if (lit) {
            return lit->getValue().getBool() == false;
        }
    }

    return false;
}

std::string QueryRewriter::extractColumnName(parser::ExprPtr expr) {
    if (!expr) return "";

    if (expr->getType() == parser::ASTNodeType::COLUMN_REF) {
        auto colRef = std::dynamic_pointer_cast<parser::ColumnRef>(expr);
        if (colRef) {
            return colRef->getColumn();
        }
    }

    return "";
}
