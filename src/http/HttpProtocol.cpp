#include "HttpProtocol.h"
#include <algorithm>
#include <string>
#include <vector>

namespace sipi::http {
std::variant<QueryParameters, QueryError> parse_query_parameters(std::string_view raw) {
    if (raw.size() > 32 * 1024) return QueryError::limit;
    QueryParameters result;
    if (raw.empty()) return result;
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const auto decode = [&](std::string_view input, std::string& output,
                            std::size_t limit) -> std::optional<QueryError> {
        for (std::size_t i = 0; i < input.size(); ++i) {
            auto c = static_cast<unsigned char>(input[i]);
            if (c == '#') return QueryError::malformed;
            if (c == '%') {
                if (input.size() - i < 3) return QueryError::malformed;
                const int high = hex(input[i + 1]), low = hex(input[i + 2]);
                if (high < 0 || low < 0) return QueryError::malformed;
                c = static_cast<unsigned char>(16 * high + low);
                i += 2;
            } else if (c == '+') c = ' ';
            if (c < 32 || c == 127) return QueryError::malformed;
            if (output.size() == limit) return QueryError::limit;
            output.push_back(static_cast<char>(c));
        }
        return std::nullopt;
    };
    std::size_t pairs = 0;
    while (true) {
        if (++pairs > 128) return QueryError::limit;
        const auto end = raw.find('&');
        const auto pair = raw.substr(0, end);
        const auto equals = pair.find('=');
        std::string name, value;
        if (const auto error = decode(pair.substr(0, equals), name, 256)) return *error;
        if (name.empty()) return QueryError::malformed;
        if (equals != std::string_view::npos) {
            if (const auto error = decode(pair.substr(equals + 1), value, 16 * 1024)) return *error;
        }
        result[std::move(name)].push_back(std::move(value));
        if (end == std::string_view::npos) break;
        raw.remove_prefix(end + 1);
    }
    return result;
}

namespace {
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front()==' ' || s.front()=='\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()==' ' || s.back()=='\t')) s.remove_suffix(1);
    return s;
}
std::string lower(std::string_view s) {
    std::string result(s);
    for (auto& c : result) if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return result;
}
/** Split separators outside quoted strings, retaining escapes for value parsing. */
std::vector<std::string_view> split(std::string_view s, char delimiter) {
    std::vector<std::string_view> parts;
    bool quoted=false, escaped=false;
    std::size_t start=0;
    for (std::size_t i=0;i<s.size();++i) {
        if (escaped) { escaped=false; continue; }
        if (quoted && s[i]=='\\') { escaped=true; continue; }
        if (s[i]=='"') quoted=!quoted;
        if (s[i]==delimiter && !quoted) { parts.push_back(trim(s.substr(start,i-start))); start=i+1; }
    }
    if (quoted || escaped) return {};
    parts.push_back(trim(s.substr(start)));
    return parts;
}
std::optional<std::string> value(std::string_view s) {
    if (s.empty()) return {};
    if (s.front()!='"') {
        if (s.find_first_of(" \t\"\\")!=s.npos) return {};
        return std::string(s);
    }
    if (s.size()<2 || s.back()!='"') return {};
    std::string result;
    for (std::size_t i=1;i+1<s.size();++i) {
        if (s[i]=='\\') {
            if (++i+1>=s.size()) return {};
        } else if (s[i]=='"') return {};
        result+=s[i];
    }
    return result;
}
/** Integer thousandths implements HTTP qvalue grammar without locale/floats. */
std::optional<int> quality(std::string_view s) {
    if (s.empty() || (s[0]!='0' && s[0]!='1')) return {};
    if (s.size()==1) return s[0]=='1' ? 1000 : 0;
    if (s[1]!='.' || s.size()>5) return {};
    int q=s[0]=='1' ? 1000 : 0, weight=100;
    for (std::size_t i=2;i<s.size();++i) {
        if (s[i]<'0' || s[i]>'9' || (s[0]=='1' && s[i]!='0')) return {};
        q+=(s[i]-'0')*weight; weight/=10;
    }
    return q;
}
struct Preference {
    int specificity=-1, parameters=-1, q=0;
    void offer(int s, int p, int weight) {
        if (s>specificity || (s==specificity && p>parameters) ||
            (s==specificity && p==parameters && weight>q))
            *this={s,p,weight};
    }
};
}

RoutedIiifRequest route_iiif_path(std::string_view path) {
    auto parsed=iiif::parse_path(path);
    if (!std::holds_alternative<iiif::ParseError>(parsed)) return {std::move(parsed),false};
    const auto candidate=iiif::parse_path(std::string(path)+"/info.json");
    const auto* base=std::get_if<iiif::InfoRequest>(&candidate);
    if (!base) return {std::move(parsed),false};
    const auto& r=base->resource;
    // Reserve explicit info paths and image-shaped suffixes. With arbitrary
    // prefixes there is unavoidable overlap; document this deterministic priority.
    if (r.identifier=="info.json") return {std::move(parsed),false};
    if (r.prefix.size()>=4) {
        const auto& region=r.prefix[r.prefix.size()-3];
        const auto dot=r.identifier.find('.');
        const auto q=std::string_view(r.identifier).substr(0,dot);
        if (region=="full" || region=="square" || region.starts_with("pct:") ||
            region.find(',')!=region.npos ||
            (dot!=r.identifier.npos && (q=="default" || q=="color" || q=="gray" || q=="bitonal")))
            return {std::move(parsed),false};
    }
    return {candidate,true};
}

std::optional<InfoMediaType> negotiate_info_type(std::optional<std::string_view> accept) {
    if (!accept) return InfoMediaType::json_ld;
    Preference json, ld;
    for (auto range : split(*accept,',')) {
        const auto parts=split(range,';');
        if (parts.empty()) continue;
        const auto type=lower(parts[0]);
        const int specificity=type=="*/*" ? 0 : type=="application/*" ? 1 : 2;
        bool matches_json=type=="application/json" || specificity<2;
        bool matches_ld=type=="application/ld+json" || specificity<2;
        bool valid=true, seen_q=false;
        int q=1000, parameters=0;
        std::vector<std::string> seen_parameters;
        for (std::size_t i=1;i<parts.size();++i) {
            const auto equal=parts[i].find('=');
            const auto name=lower(trim(parts[i].substr(0,equal)));
            if (seen_q && name!="q") continue; // Accept extensions.
            if (equal==parts[i].npos) { valid=false; break; }
            const auto raw=trim(parts[i].substr(equal+1));
            if (name=="q") {
                const auto weight=quality(raw);
                if (seen_q || !weight) { valid=false; break; }
                q=*weight; seen_q=true; continue;
            }
            const auto v=value(raw);
            if (!v || name.empty() ||
                std::find(seen_parameters.begin(),seen_parameters.end(),name)!=seen_parameters.end()) {
                valid=false; break;
            }
            seen_parameters.push_back(name);
            ++parameters;
            if (name=="charset") {
                if (lower(*v)!="utf-8") matches_json=matches_ld=false;
            } else if (name=="profile") {
                matches_json=false;
                if (*v!="http://iiif.io/api/image/3/context.json") matches_ld=false;
            } else matches_json=matches_ld=false;
        }
        if (!valid) continue;
        if (matches_json) json.offer(specificity,parameters,q);
        if (matches_ld) ld.offer(specificity,parameters,q);
    }
    if (json.q==0 && ld.q==0) return {};
    return ld.q>=json.q ? InfoMediaType::json_ld : InfoMediaType::json;
}
bool allowed_cors_preflight(std::string_view method,std::string_view headers) {
    if (method!="GET" && method!="HEAD") return false;
    if (trim(headers).empty()) return true;
    for (const auto header : split(headers,',')) {
        const auto name=lower(header);
        if (name!="authorization" && name!="accept" && name!="content-type") return false;
    }
    return !split(headers,',').empty();
}
} // namespace sipi::http
