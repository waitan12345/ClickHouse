#include <Parsers/ParserCreateUserQuery.h>
#include <Parsers/ASTCreateUserQuery.h>
#include <Parsers/CommonParsers.h>
#include <Parsers/parseUserName.h>
#include <Parsers/parseIdentifierOrStringLiteral.h>
#include <Parsers/ExpressionElementParsers.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTExtendedRoleSet.h>
#include <Parsers/ParserExtendedRoleSet.h>
#include <Parsers/ASTSettingsProfileElement.h>
#include <Parsers/ParserSettingsProfileElement.h>
#include <ext/range.h>
#include <boost/algorithm/string/predicate.hpp>


namespace DB
{
namespace ErrorCodes
{
}


namespace
{
    bool parseRenameTo(IParserBase::Pos & pos, Expected & expected, String & new_name, String & new_host_pattern)
    {
        return IParserBase::wrapParseImpl(pos, [&]
        {
            if (!ParserKeyword{"RENAME TO"}.ignore(pos, expected))
                return false;

            return parseUserName(pos, expected, new_name, new_host_pattern);
        });
    }


    bool parseByPassword(IParserBase::Pos & pos, Expected & expected, String & password)
    {
        return IParserBase::wrapParseImpl(pos, [&]
        {
            if (!ParserKeyword{"BY"}.ignore(pos, expected))
                return false;

            ASTPtr ast;
            if (!ParserStringLiteral{}.parse(pos, ast, expected))
                return false;

            password = ast->as<const ASTLiteral &>().value.safeGet<String>();
            return true;
        });
    }


    bool parseAuthentication(IParserBase::Pos & pos, Expected & expected, std::optional<Authentication> & authentication)
    {
        return IParserBase::wrapParseImpl(pos, [&]
        {
            if (!ParserKeyword{"IDENTIFIED"}.ignore(pos, expected))
                return false;

            if (!ParserKeyword{"WITH"}.ignore(pos, expected))
            {
                String password;
                if (!parseByPassword(pos, expected, password))
                    return false;

                authentication = Authentication{Authentication::SHA256_PASSWORD};
                authentication->setPassword(password);
                return true;
            }

            if (ParserKeyword{"PLAINTEXT_PASSWORD"}.ignore(pos, expected))
            {
                String password;
                if (!parseByPassword(pos, expected, password))
                    return false;

                authentication = Authentication{Authentication::PLAINTEXT_PASSWORD};
                authentication->setPassword(password);
                return true;
            }

            if (ParserKeyword{"SHA256_PASSWORD"}.ignore(pos, expected))
            {
                String password;
                if (!parseByPassword(pos, expected, password))
                    return false;

                authentication = Authentication{Authentication::SHA256_PASSWORD};
                authentication->setPassword(password);
                return true;
            }

            if (ParserKeyword{"SHA256_HASH"}.ignore(pos, expected))
            {
                String hash;
                if (!parseByPassword(pos, expected, hash))
                    return false;

                authentication = Authentication{Authentication::SHA256_PASSWORD};
                authentication->setPasswordHashHex(hash);
                return true;
            }

            if (ParserKeyword{"DOUBLE_SHA1_PASSWORD"}.ignore(pos, expected))
            {
                String password;
                if (!parseByPassword(pos, expected, password))
                    return false;

                authentication = Authentication{Authentication::DOUBLE_SHA1_PASSWORD};
                authentication->setPassword(password);
                return true;
            }

            if (ParserKeyword{"DOUBLE_SHA1_HASH"}.ignore(pos, expected))
            {
                String hash;
                if (!parseByPassword(pos, expected, hash))
                    return false;

                authentication = Authentication{Authentication::DOUBLE_SHA1_PASSWORD};
                authentication->setPasswordHashHex(hash);
                return true;
            }

            if (!ParserKeyword{"NO_PASSWORD"}.ignore(pos, expected))
                return false;

            authentication = Authentication{Authentication::NO_PASSWORD};
            return true;
        });
    }


    bool parseHosts(IParserBase::Pos & pos, Expected & expected, const char * prefix, std::optional<AllowedClientHosts> & hosts)
    {
        return IParserBase::wrapParseImpl(pos, [&]
        {
            if (prefix && !ParserKeyword{prefix}.ignore(pos, expected))
                return false;

            if (!ParserKeyword{"HOST"}.ignore(pos, expected))
                return false;

            if (ParserKeyword{"ANY"}.ignore(pos, expected))
            {
                if (!hosts)
                    hosts.emplace();
                hosts->addAnyHost();
                return true;
            }

            if (ParserKeyword{"NONE"}.ignore(pos, expected))
            {
                if (!hosts)
                    hosts.emplace();
                return true;
            }

            AllowedClientHosts new_hosts;
            do
            {
                if (ParserKeyword{"LOCAL"}.ignore(pos, expected))
                {
                    new_hosts.addLocalHost();
                }
                else if (ParserKeyword{"NAME REGEXP"}.ignore(pos, expected))
                {
                    ASTPtr ast;
                    if (!ParserStringLiteral{}.parse(pos, ast, expected))
                        return false;

                    new_hosts.addNameRegexp(ast->as<const ASTLiteral &>().value.safeGet<String>());
                }
                else if (ParserKeyword{"NAME"}.ignore(pos, expected))
                {
                    ASTPtr ast;
                    if (!ParserStringLiteral{}.parse(pos, ast, expected))
                        return false;

                    new_hosts.addName(ast->as<const ASTLiteral &>().value.safeGet<String>());
                }
                else if (ParserKeyword{"IP"}.ignore(pos, expected))
                {
                    ASTPtr ast;
                    if (!ParserStringLiteral{}.parse(pos, ast, expected))
                        return false;

                    new_hosts.addSubnet(ast->as<const ASTLiteral &>().value.safeGet<String>());
                }
                else if (ParserKeyword{"LIKE"}.ignore(pos, expected))
                {
                    ASTPtr ast;
                    if (!ParserStringLiteral{}.parse(pos, ast, expected))
                        return false;

                    new_hosts.addLikePattern(ast->as<const ASTLiteral &>().value.safeGet<String>());
                }
                else
                    return false;
            }
            while (ParserToken{TokenType::Comma}.ignore(pos, expected));

            if (!hosts)
                hosts.emplace();
            hosts->add(new_hosts);
            return true;
        });
    }


    bool parseDefaultRoles(IParserBase::Pos & pos, Expected & expected, bool id_mode, std::shared_ptr<ASTExtendedRoleSet> & default_roles)
    {
        return IParserBase::wrapParseImpl(pos, [&]
        {
            if (!ParserKeyword{"DEFAULT ROLE"}.ignore(pos, expected))
                return false;

            ASTPtr ast;
            if (!ParserExtendedRoleSet{}.enableCurrentUserKeyword(false).useIDMode(id_mode).parse(pos, ast, expected))
                return false;

            default_roles = typeid_cast<std::shared_ptr<ASTExtendedRoleSet>>(ast);
            return true;
        });
    }


    bool parseSettings(IParserBase::Pos & pos, Expected & expected, bool id_mode, std::shared_ptr<ASTSettingsProfileElements> & settings)
    {
        return IParserBase::wrapParseImpl(pos, [&]
        {
            if (!ParserKeyword{"SETTINGS"}.ignore(pos, expected))
                return false;

            ASTPtr new_settings_ast;
            if (!ParserSettingsProfileElements{}.useIDMode(id_mode).parse(pos, new_settings_ast, expected))
                return false;

            if (!settings)
                settings = std::make_shared<ASTSettingsProfileElements>();
            const auto & new_settings = new_settings_ast->as<const ASTSettingsProfileElements &>();
            settings->elements.insert(settings->elements.end(), new_settings.elements.begin(), new_settings.elements.end());
            return true;
        });
    }
}


bool ParserCreateUserQuery::parseImpl(Pos & pos, ASTPtr & node, Expected & expected)
{
    bool alter = false;
    if (attach_mode)
    {
        if (!ParserKeyword{"ATTACH USER"}.ignore(pos, expected))
            return false;
    }
    else
    {
        if (ParserKeyword{"ALTER USER"}.ignore(pos, expected))
            alter = true;
        else if (!ParserKeyword{"CREATE USER"}.ignore(pos, expected))
            return false;
    }

    bool if_exists = false;
    bool if_not_exists = false;
    bool or_replace = false;
    if (alter)
    {
        if (ParserKeyword{"IF EXISTS"}.ignore(pos, expected))
            if_exists = true;
    }
    else
    {
        if (ParserKeyword{"IF NOT EXISTS"}.ignore(pos, expected))
            if_not_exists = true;
        else if (ParserKeyword{"OR REPLACE"}.ignore(pos, expected))
            or_replace = true;
    }

    String name;
    String host_pattern;
    if (!parseUserName(pos, expected, name, host_pattern))
        return false;

    String new_name;
    String new_host_pattern;
    std::optional<Authentication> authentication;
    std::optional<AllowedClientHosts> hosts;
    std::optional<AllowedClientHosts> add_hosts;
    std::optional<AllowedClientHosts> remove_hosts;
    std::shared_ptr<ASTExtendedRoleSet> default_roles;
    std::shared_ptr<ASTSettingsProfileElements> settings;

    while (true)
    {
        if (!authentication && parseAuthentication(pos, expected, authentication))
            continue;

        if (parseHosts(pos, expected, nullptr, hosts))
            continue;

        if (parseSettings(pos, expected, attach_mode, settings))
            continue;

        if (!default_roles && parseDefaultRoles(pos, expected, attach_mode, default_roles))
            continue;

        if (alter)
        {
            if (new_name.empty() && parseRenameTo(pos, expected, new_name, new_host_pattern))
                continue;

            if (parseHosts(pos, expected, "ADD", add_hosts) || parseHosts(pos, expected, "REMOVE", remove_hosts))
                continue;
        }

        break;
    }

    if (!hosts)
    {
        if (!alter)
            hosts.emplace().addLikePattern(host_pattern);
        else if (alter && !new_name.empty())
            hosts.emplace().addLikePattern(new_host_pattern);
    }

    auto query = std::make_shared<ASTCreateUserQuery>();
    node = query;

    query->alter = alter;
    query->attach = attach_mode;
    query->if_exists = if_exists;
    query->if_not_exists = if_not_exists;
    query->or_replace = or_replace;
    query->name = std::move(name);
    query->new_name = std::move(new_name);
    query->authentication = std::move(authentication);
    query->hosts = std::move(hosts);
    query->add_hosts = std::move(add_hosts);
    query->remove_hosts = std::move(remove_hosts);
    query->default_roles = std::move(default_roles);
    query->settings = std::move(settings);

    return true;
}
}
