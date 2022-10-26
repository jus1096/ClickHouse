#include <TableFunctions/TableFunctionFactory.h>

#include <Interpreters/Context.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <IO/WriteHelpers.h>
#include <Parsers/ASTFunction.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_FUNCTION;
    extern const int LOGICAL_ERROR;
}

static const NameSet table_functions_allowed_in_readonly_mode
{
    "null", "view", "viewIfPermitted", "numbers", "numbers_mt", "generateRandom", "values", "cluster", "clusterAllReplicas"
};

void TableFunctionFactory::registerFunction(
    const std::string & name, TableFunctionCreator creator, Documentation doc, CaseSensitiveness case_sensitiveness)
{
    bool allowed_in_readonly_mode = table_functions_allowed_in_readonly_mode.contains(name);

    if (!table_functions.emplace(name, TableFunctionFactoryData{creator, {doc,allowed_in_readonly_mode}}).second)
        throw Exception("TableFunctionFactory: the table function name '" + name + "' is not unique",
            ErrorCodes::LOGICAL_ERROR);

    if (case_sensitiveness == CaseInsensitive
        && !case_insensitive_table_functions.emplace(Poco::toLower(name), TableFunctionFactoryData{creator, {doc,allowed_in_readonly_mode}}).second)
        throw Exception("TableFunctionFactory: the case insensitive table function name '" + name + "' is not unique",
                        ErrorCodes::LOGICAL_ERROR);
}

TableFunctionPtr TableFunctionFactory::get(
    const ASTPtr & ast_function,
    ContextPtr context) const
{
    const auto * table_function = ast_function->as<ASTFunction>();
    auto res = tryGet(table_function->name, context);
    if (!res)
    {
        auto hints = getHints(table_function->name);
        if (!hints.empty())
            throw Exception(ErrorCodes::UNKNOWN_FUNCTION, "Unknown table function {}. Maybe you meant: {}", table_function->name , toString(hints));
        else
            throw Exception(ErrorCodes::UNKNOWN_FUNCTION, "Unknown table function {}", table_function->name);
    }

    res->parseArguments(ast_function, context);
    return res;
}

TableFunctionPtr TableFunctionFactory::tryGet(
        const std::string & name_param,
        ContextPtr) const
{
    String name = getAliasToOrName(name_param);
    TableFunctionPtr res;

    auto it = table_functions.find(name);
    if (table_functions.end() != it)
    {
        res = it->second.creator();
    }
    else
    {
        it = case_insensitive_table_functions.find(Poco::toLower(name));
        if (case_insensitive_table_functions.end() != it)
            res = it->second.creator();
    }

    if (!res)
        return nullptr;

    if (CurrentThread::isInitialized())
    {
        auto query_context = CurrentThread::get().getQueryContext();
        if (query_context && query_context->getSettingsRef().log_queries)
            query_context->addQueryFactoriesInfo(Context::QueryLogFactories::TableFunction, name);
    }

    return res;
}

bool TableFunctionFactory::isTableFunctionName(const std::string & name) const
{
    return table_functions.contains(name);
}

std::optional<TableFunctionProperties> TableFunctionFactory::tryGetProperties(const String & name) const
{
    return tryGetPropertiesImpl(name);
}

std::optional<TableFunctionProperties> TableFunctionFactory::tryGetPropertiesImpl(const String & name_param) const
{
    String name = getAliasToOrName(name_param);
    Value found;

    /// Find by exact match.
    if (auto it = table_functions.find(name); it != table_functions.end())
    {
        found = it->second;
    }

    if (auto jt = case_insensitive_table_functions.find(Poco::toLower(name)); jt != case_insensitive_table_functions.end())
        found = jt->second;

    if (found.creator)
        return found.properties;

    return {};
}

TableFunctionFactory & TableFunctionFactory::instance()
{
    static TableFunctionFactory ret;
    return ret;
}

}
