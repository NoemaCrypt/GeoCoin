#include "geocoin/nft/geodesy.hpp"
#include "geocoin/nft/geometry_codec.hpp"
#include "geocoin/nft/nft_validator.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/bytes.hpp"
#include <GeographicLib/Ellipsoid.hpp>
#include <GeographicLib/Constants.hpp>
#include <GeographicLib/Math.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
namespace g = geocoin::nft::coordinates;
namespace powgeo = geocoin::consensus::geo;
using Point = g::Point;
using Ring = g::Ring;
using Geometry = g::Geometry;
using Json = nlohmann::json;
int checks = 0;
Point p(double latitude, double longitude) {
    return {static_cast<int64_t>(std::llround(latitude * 1e9)), static_cast<int64_t>(std::llround(longitude * 1e9))};
}
Geometry shape(Ring ring) { return Geometry{{{std::move(ring), {}}}}; }
Geometry square(double south, double west, double size = 0.1) {
    return shape({p(south, west), p(south, west + size), p(south + size, west + size), p(south + size, west)});
}
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        std::cerr << message << ": " << actual << " vs " << expected << '\n';
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
void distances() {
    near(*g::geodesic_distance_m(p(0,0), p(0,1)), 111319.49079327357, 1e-7, "equatorial degree");
    near(*g::geodesic_distance_m(p(0,0), p(1,0)), 110574.38855779878, 1e-7, "meridional degree");
    near(*g::geodesic_distance_m(p(0,179.9), p(0,-179.9)), 22263.898158654716, 1e-7, "dateline shortest path");
    near(*g::geodesic_distance_m(p(-41.32,174.81), p(40.96,-5.5)), 19959679.26735382, 1e-6, "nearly antipodal inverse");
    near(*g::geodesic_distance_m(p(90,123), p(-90,-70)), 20003931.458625447, 1e-6, "pole to pole");
    near(*g::geodesic_distance_m(p(90,0), p(90,170)), 0, 1e-9, "pole longitude alias");
    near(*g::geodesic_distance_m(p(12,180), p(12,-180)), 0, 1e-9, "dateline alias");
    check(!g::geodesic_distance_m({90'000'000'001LL,0}, p(0,0)), "latitude range in int64 nanodegrees");
    check(!g::geodesic_distance_m(p(0,0), {0,std::numeric_limits<int64_t>::max()}), "longitude range");
    const auto east = *g::geodesic_destination(p(0,0),90,111319.49079327357);
    check(east == p(0,1), "direct inverse roundtrip");
    check(*g::geodesic_destination(p(0,179.9),90,22263.898158654716) == p(0,-179.9), "direct dateline");
    const auto from_pole = *g::geodesic_destination(p(90,170),135,1000);
    near(*g::geodesic_distance_m(p(90,170),from_pole),1000,0.00006,"nanodegree direct pole precision");
    check(!g::geodesic_destination(p(0,0),90,-1), "negative direct distance");
    const g::PreparedGeometry land(square(0,0));
    const auto query = p(-0.00009,0.05);
    near(*land.distance_m(query),9.95168482394,0.00001,"distance to middle of long equatorial boundary");
    check(*land.distance_m(query,20) <= 20,"radius query finds edge far from vertices");
    const auto vertex = p(0,0);
    near(*land.distance_m(vertex),0,0,"distance on boundary");
}
void metrics_and_canonicalization() {
    Geometry triangle = shape({p(2,1),p(1,2),p(3,3)});
    const auto original = *g::measure_geometry(triangle);
    near(original.area_sqm(),18454562325.45119,0.001,"upstream GeographicLib area vector");
    auto rotated = triangle;
    auto& ring = rotated.polygons[0].exterior;
    std::rotate(ring.begin(),ring.begin()+1,ring.end());
    std::reverse(ring.begin(),ring.end());
    ring.push_back(ring.front());
    check(g::canonicalize_geometry(rotated) == g::canonicalize_geometry(triangle),"ring rotation, winding and closure canonicalize");
    geocoin::nft::NFTMetadata a{},b{};
    a.geometry=triangle;b.geometry=rotated;
    a.area_bits=b.area_bits=123; // Unselected opaque metadata field remains signed.
    check(a.serialize()==b.serialize() && a.calculate_hash()==b.calculate_hash(),"one canonical wire identity for equivalent rings");
    const auto decoded=geocoin::nft::NFTMetadata::deserialize(a.serialize());
    check(decoded.geometry==g::canonicalize_geometry(triangle),"64-bit nanodegree geometry wire roundtrip");
    check(decoded.area_bits==123,"area_bits retained without reinterpretation");
    auto noncanonical=a.serialize();
    // Four empty string lengths, then part/ring/vertex CompactSize counts.
    check(noncanonical[4]==1 && noncanonical[5]==1 && noncanonical[6]==3,"canonical geometry wire fixture layout");
    std::swap_ranges(noncanonical.begin()+7,noncanonical.begin()+23,noncanonical.begin()+23);
    bool rejected=false;
    try{geocoin::nft::NFTMetadata::deserialize(noncanonical);}catch(const std::exception&){rejected=true;}
    check(rejected,"wire decoder rejects a noncanonical ring rather than silently rewriting its signed bytes");
    auto fine=square(48.123456789,8.987654321,0.00001);
    a.geometry=fine;
    check(geocoin::nft::NFTMetadata::deserialize(a.serialize()).geometry==g::canonicalize_geometry(fine),"sub-microdegree precision preserved");
    check(g::area_square_centimetres(fine).value() > 0,"sub-square-metre official area unit");
    Geometry seam=shape({p(-1,179.9),p(-1,-179.9),p(1,-179.9),p(1,179.9)});
    auto centred=shape({p(-1,-0.1),p(-1,0.1),p(1,0.1),p(1,-0.1)});
    near(g::measure_geometry(seam)->area_sqm(),g::measure_geometry(centred)->area_sqm(),0.01,"longitude-invariant area");
    auto aliases=shape({p(0,180),p(0,-179.9),p(0.1,-179.9),p(0.1,-180)});
    auto normalized=aliases;normalized.polygons[0].exterior[0].lon_ndeg=-180'000'000'000LL;
    check(g::canonicalize_geometry(aliases)==g::canonicalize_geometry(normalized),"dateline alias canonicalization");
    auto cap=shape({p(90,70),p(89,0),p(89,120)});
    auto pole_alias=cap;pole_alias.polygons[0].exterior[0].lon_ndeg=-170'000'000'000LL;
    check(g::canonicalize_geometry(cap)==g::canonicalize_geometry(pole_alias),"pole longitude canonicalization");
}
void topology_and_complex_parts() {
    auto land=square(0,0);
    check(bool(g::validate_geometry(land)),"simple parcel");
    const g::PreparedGeometry prepared(land);
    check(prepared.locate(p(0.05,0.05))==g::PointLocation::inside,"ordinary inside");
    check(prepared.locate(p(0,0.05))==g::PointLocation::boundary,"ordinary boundary");
    check(prepared.locate(p(-0.05,-179.95))==g::PointLocation::outside,"antipodal winding outside");
    check(!g::validate_geometry(shape({p(0,0),p(0.1,0.1),p(0,0.1),p(0.1,0)})),"bow tie rejected");
    auto touch=shape({p(0,0),p(0.1,0),p(0.1,0.1),p(0.05,0),p(0,0.1)});
    const auto diagnosis=g::validate_geometry(touch);
    check(!diagnosis && diagnosis.issue->code=="self_intersection" && diagnosis.issue->edge && diagnosis.issue->other_edge,
        "self-touch names both offending edges");
    check(!g::validate_geometry(shape({p(0,0),p(0,180),p(1,1)})),"ambiguous antipodal edge");
    auto polar=shape({p(89,0),p(89,120),p(89,-120)});
    check(bool(g::validate_geometry(polar)),"polar parcel");
    check(g::PreparedGeometry(polar).locate(p(90,75))==g::PointLocation::inside,"north pole inside");
    check(g::PreparedGeometry(polar).locate(p(-90,0))==g::PointLocation::outside,"south pole outside");
    check(!g::geometry_interiors_overlap(land,square(0,0.1)),"neighbours share a boundary");
    check(g::geometry_interiors_overlap(land,land),"identical parcel overlaps");
    auto donut=square(0,0,1);
    donut.polygons[0].holes.push_back(square(0.2,0.2,0.6).polygons[0].exterior);
    check(bool(g::validate_geometry(donut)),"interior hole supported");
    const g::PreparedGeometry hollow(donut);
    check(hollow.locate(p(0.5,0.5))==g::PointLocation::outside,"hole is excluded");
    check(hollow.locate(p(0.5,0.1))==g::PointLocation::inside,"land surrounding hole");
    check(*hollow.distance_m(p(0.5,0.5))>30000,"hole radius measured to hole boundary");
    check(!g::geometry_interiors_overlap(donut,square(0.4,0.4,0.1)),"another parcel may lie in an excluded hole");
    check(g::geometry_interiors_overlap(donut,square(0.1,0.1,0.2)),"partially crossing a hole boundary overlaps land");
    auto bad=donut;bad.polygons[0].holes[0]=square(2,2).polygons[0].exterior;
    check(!g::validate_geometry(bad),"outside hole rejected");
    auto multipart=land;multipart.polygons.push_back(square(2,2).polygons[0]);
    check(bool(g::validate_geometry(multipart)),"separate polygon parts supported");
    check(g::PreparedGeometry(multipart).locate(p(2.05,2.05))==g::PointLocation::inside,"point in second part");
    auto swap=multipart;std::reverse(swap.polygons.begin(),swap.polygons.end());
    check(g::canonicalize_geometry(swap)==g::canonicalize_geometry(multipart),"part ordering canonicalized");
    swap.polygons[1]=swap.polygons[0];check(!g::validate_geometry(swap),"overlapping parts rejected");
    auto several=square(0,0,4);
    several.polygons[0].holes={square(0.2,0.2,0.2).polygons[0].exterior,square(2,2,0.2).polygons[0].exterior};
    auto permuted=several;std::reverse(permuted.polygons[0].holes.begin(),permuted.polygons[0].holes.end());
    for(auto& hole:permuted.polygons[0].holes)std::reverse(hole.begin(),hole.end());
    check(bool(g::validate_geometry(several)) && g::canonicalize_geometry(permuted)==g::canonicalize_geometry(several),
        "hole order and winding have one canonical representation");
}
void geojson() {
    auto land=square(0,0);
    auto native=g::geometry_to_json(land);
    check(g::geometry_from_json(native)==g::canonicalize_geometry(land),"native JSON roundtrip");
    const auto display=g::geometry_to_geojson(land,1);
    check(display["type"]=="Polygon","single display polygon");
    check(display["coordinates"][0].front()==display["coordinates"][0].back(),"GeoJSON rings closed");
    check(bool(g::validate_geometry(g::geometry_from_geojson(display))),"GeoJSON import validates");
    const Geometry seam=shape({p(-1,179.9),p(-1,-179.9),p(1,-179.9),p(1,179.9)});
    const auto split=g::geometry_to_geojson(seam,10);
    check(split["type"]=="MultiPolygon" && split["coordinates"].size()==2,"dateline cut into two parts");
    auto polar=shape({p(89,0),p(89,120),p(89,-120)});
    check(g::geometry_to_geojson(polar,10).contains("coordinates"),"polar GeoJSON exports");
    auto hollow=square(-1,179,2); // Rewrite wrapped east vertices explicitly.
    for(auto& point:hollow.polygons[0].exterior)if(point.lon_ndeg>180'000'000'000LL)point.lon_ndeg-=360'000'000'000LL;
    hollow.polygons[0].holes.push_back(shape({p(-0.5,179.5),p(-0.5,-179.5),p(0.5,-179.5),p(0.5,179.5)}).polygons[0].exterior);
    const auto holed=g::geometry_to_geojson(hollow,10);
    check(holed["type"]=="MultiPolygon","dateline-cut holes export");
    auto unclosed=display;unclosed["coordinates"][0].erase(unclosed["coordinates"][0].size()-1);
    bool threw=false;try{g::geometry_from_geojson(unclosed);}catch(const g::GeometryError& issue){threw=issue.issue.code=="unclosed_ring";}
    check(threw,"unclosed GeoJSON ring diagnosed");
    auto fractional=native;fractional["parts"][0]["exterior"][1]["lat_ndeg"]=0.5;
    threw=false;try{g::geometry_from_json(fractional);}catch(const g::GeometryError&){threw=true;}check(threw,"native fractional coordinates rejected");
    const Json parallel{{"type","Polygon"},{"coordinates",Json::array({Json::array({
        Json::array({0,50}),Json::array({1,50}),Json::array({1,51}),Json::array({0,51}),Json::array({0,50})})})}};
    const auto imported=g::geometry_from_geojson(parallel,1);
    const auto coarse=g::geometry_from_geojson(parallel,1000);
    check(imported.polygons[0].exterior.size()>coarse.polygons[0].exterior.size(),"GeoJSON linear parallels are adaptively approximated");
    check(*g::PreparedGeometry(imported).distance_m(p(50,0.5))<=1.001,"GeoJSON import retains straight longitude/latitude edge semantics");
    auto uncut=parallel;
    uncut["coordinates"][0]=Json::array({Json::array({179.9,0}),Json::array({-179.9,0}),
        Json::array({-179.9,1}),Json::array({179.9,0})});
    threw=false;try{g::geometry_from_geojson(uncut);}catch(const g::GeometryError& issue){threw=issue.issue.code=="uncut_antimeridian";}
    check(threw,"uncut GeoJSON dateline jumps require explicit splitting");
    auto bad=native;
    bad["parts"][0]["exterior"][2]=bad["parts"][0]["exterior"][1];
    threw=false;try{g::geometry_from_json(bad);}catch(const g::GeometryError& issue){
        threw=issue.issue.code=="duplicate_position" && issue.issue.part==0 && issue.issue.ring==0 && issue.issue.vertex==2;}
    check(threw,"diagnostics refer to caller vertex indices before canonical sorting");
    bad=native;bad["parts"][0]["exterior"][0]["lat_ndeg"]=(std::numeric_limits<int64_t>::min)();
    threw=false;try{g::geometry_from_json(bad);}catch(const g::GeometryError& issue){threw=issue.issue.code=="coordinate_range";}
    check(threw,"signed coordinate extrema are diagnosed without arithmetic overflow");
    for(const auto& value:{split,holed,g::geometry_to_geojson(polar,10)}) {
        const auto parts=value["type"]=="Polygon"?Json::array({value["coordinates"]}):value["coordinates"];
        for(const auto& part:parts)for(const auto& ring:part){
            check(ring.front()==ring.back(),"every clipped GeoJSON ring is closed");
            for(size_t i=1;i<ring.size();++i){
                const double lon=ring[i][0],lat=ring[i][1],previous=ring[i-1][0];
                check(lon>=-180 && lon<=180 && lat>=-90 && lat<=90,"display positions remain in RFC7946 ranges");
                check(std::abs(lon-previous)<=180 || std::abs(lat)==90,"display rings contain no uncut dateline jump");
            }
        }
    }
}
void registry_and_pol() {
    geocoin::nft::NFTRegistryImpl registry;
    geocoin::nft::NFTCreationTransaction tx{};tx.nft_id[0]=1;tx.metadata.geometry=square(0,0);tx.metadata.created_at=1700000000;
    check(registry.register_nft(tx),"registry stores canonical geometry");
    check(registry.prepared_geometry(tx.nft_id)==registry.prepared_geometry(tx.nft_id),"prepared geometry is reused across lookups");
    const auto query=p(-0.00009,0.05);
    check(registry.find_by_location(query.lat_ndeg,query.lon_ndeg,20).size()==1,"registry radius finds middle of edge");
    check(registry.find_by_location(query.lat_ndeg,query.lon_ndeg,9).empty(),"registry radius excludes points outside buffer");
    geocoin::nft::ProofOfLocationData pol{};pol.latitude_ndeg=query.lat_ndeg;pol.longitude_ndeg=query.lon_ndeg;
    pol.timestamp_ms=1700000000000ULL;pol.gps_accuracy_cm=1000;
    check(geocoin::nft::NFTValidator::validate_proof_of_location(pol,tx.metadata),"PoL buffer includes whole edge");
    check(pol.serialize().size()==29 && geocoin::nft::ProofOfLocationData::deserialize(pol.serialize()).latitude_ndeg==query.lat_ndeg,
        "PoL nanodegree wire precision");
    for(int i=0;i<1000;++i){tx.nft_id[0]=static_cast<uint8_t>(i);tx.nft_id[1]=static_cast<uint8_t>(i>>8);tx.nft_id[2]=1;
        tx.metadata.geometry=square(20+(i/100)*2,-170+(i%100)*3,0.01);registry.register_nft(tx);}
    check(registry.find_by_location(query.lat_ndeg,query.lon_ndeg,20).size()==1,"spatial index preserves radius results among distant parcels");
    geocoin::Hash256 original{};original[0]=1;
    check(registry.burn_nft(original),"burn indexed parcel");
    check(registry.find_by_location(query.lat_ndeg,query.lon_ndeg,20).empty(),"burn removes geometry and cached entries");
    check(!registry.prepared_geometry(original),"burn drops the prepared cache entry");
    tx.nft_id=original;tx.metadata.geometry=square(2,2);
    check(registry.register_nft(tx),"undo can restore an indexed entry");
    check(registry.find_by_location(p(2.05,2.05).lat_ndeg,p(2.05,2.05).lon_ndeg,0).size()==1 &&
        registry.find_by_location(query.lat_ndeg,query.lon_ndeg,20).empty(),"restored registry index uses current geometry");
    tx.nft_id[2]=2;tx.metadata.geometry=shape({p(-0.1,179.9),p(-0.1,-179.9),p(0.1,-179.9),p(0.1,179.9)});
    check(registry.register_nft(tx),"index a small dateline parcel");
    check(registry.find_by_location(0,180'000'000'000LL,0).size()==1 &&
        registry.find_by_location(0,-180'000'000'000LL,0).size()==1,"spatial index finds both physical dateline aliases");
}
void geometry_limits_and_metrics() {
    Geometry limit;
    limit.polygons.clear();
    for (size_t part = 0; part < 16; ++part) {
        const double south = 10 + 2 * part;
        auto polygon = square(south, 10, 1).polygons.front();
        for (size_t hole = 0; hole < 3; ++hole)
            polygon.holes.push_back(square(south + 0.1 + 0.25 * hole, 10.1, 0.1).polygons.front().exterior);
        limit.polygons.push_back(std::move(polygon));
    }
    check(bool(g::validate_geometry(limit)), "v1 accepts exactly 16 parts, 64 rings and 256 vertices");
    const auto canonical = g::canonicalize_geometry(limit);
    check(g::geometry_from_json(g::geometry_to_json(limit)) == canonical, "native JSON accepts all v1 geometry limits together");
    geocoin::nft::NFTMetadata metadata{};
    metadata.geometry = limit;
    check(geocoin::nft::NFTMetadata::deserialize(metadata.serialize()).geometry == canonical,
        "wire decoder accepts the same geometry boundary as native JSON");

    const g::PreparedGeometry prepared(limit);
    const auto metrics = *g::measure_geometry(canonical);
    check(prepared.metrics().signed_area_sqm == metrics.signed_area_sqm &&
        prepared.metrics().perimeter_m == metrics.perimeter_m, "prepared metrics retain multipart hole accumulation order");
    check(prepared.metrics().area_sqcm() == g::area_square_centimetres(canonical),
        "cached and direct official areas agree at maximum geometry size");

    auto excessive = limit;
    excessive.polygons.back().exterior.insert(excessive.polygons.back().exterior.begin() + 1, p(40, 10.5));
    check(!g::validate_geometry(excessive), "validation rejects vertex 257 across all rings");
    bool rejected = false;
    try { g::geometry_from_json(g::geometry_to_json(excessive)); }
    catch (const g::GeometryError& error) { rejected = error.issue.code == "vertex_limit"; }
    check(rejected, "native JSON rejects the same global vertex excess");
    metadata.geometry = excessive;
    rejected = false;
    try { geocoin::nft::NFTMetadata::deserialize(metadata.serialize()); }
    catch (const std::exception&) { rejected = true; }
    check(rejected, "wire decoder rejects the same global vertex excess");

    const auto tiny = [](double area) { return g::PolygonMetrics{area, 0}.area_sqcm(); };
    check(tiny(0.0000499) == 0 && tiny(0.00005) == 1 && tiny(-0.00005) == 1,
        "official area rounds half a square centimetre up independently of winding");
    check(!tiny(std::numeric_limits<double>::infinity()) && !tiny(std::numeric_limits<double>::quiet_NaN()) &&
        !tiny(1e20), "non-finite and overflowing official areas have no integer representation");
}

void codec_diagnostics_and_rounding() {
    auto parcel = square(2, 2);
    parcel.polygons.front().holes.push_back(square(2.02, 2.02, 0.02).polygons.front().exterior);
    const auto native = g::geometry_to_json(parcel);
    const auto display = g::geometry_to_geojson(parcel);
    const auto imported = g::geometry_from_geojson(display);
    auto bad_native = native;
    bad_native["parts"].push_back(bad_native["parts"][0]);
    bad_native["parts"][1]["holes"][0][2]["lat_ndeg"] = "north";
    auto bad_display = Json{{"type", "MultiPolygon"}, {"coordinates", Json::array({display["coordinates"], display["coordinates"]})}};
    bad_display["coordinates"][1][1][2][0] = "east";

    for (int mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        std::fesetround(mode);
        const bool native_ok = g::geometry_from_json(native) == g::canonicalize_geometry(parcel);
        const bool native_restored = std::fegetround() == mode;
        const bool import_ok = g::geometry_from_geojson(display) == imported;
        const bool import_restored = std::fegetround() == mode;
        const bool export_ok = g::geometry_to_geojson(parcel) == display;
        const bool export_restored = std::fegetround() == mode;
        bool native_issue = false, display_issue = false;
        try { g::geometry_from_json(bad_native); }
        catch (const g::GeometryError& error) {
            native_issue = error.issue.code == "coordinate_type" && error.issue.part == 1 &&
                error.issue.ring == 1 && error.issue.vertex == 2 && std::fegetround() == mode;
        }
        try { g::geometry_from_geojson(bad_display); }
        catch (const g::GeometryError& error) {
            display_issue = error.issue.code == "position_format" && error.issue.part == 1 &&
                error.issue.ring == 1 && error.issue.vertex == 2 && std::fegetround() == mode;
        }
        std::fesetround(FE_TONEAREST);
        check(native_ok && native_restored, "native parsing preserves the caller rounding mode");
        check(import_ok && import_restored && export_ok && export_restored,
            "GeoJSON import and export are independent of caller rounding mode");
        check(native_issue && display_issue, "both codecs retain part, hole and vertex diagnostics while restoring rounding on failure");
    }
}

void global_and_rounding() {
    const auto land=square(0,0);
    const auto baseline=g::area_square_centimetres(land);
    for(int mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){
        std::fesetround(mode);const auto area=g::area_square_centimetres(land);bool restored=std::fegetround()==mode;std::fesetround(FE_TONEAREST);
        check(restored && area==baseline,"area independent of caller rounding mode");}
    std::vector<std::future<bool>> jobs;
    for(int i=0;i<4;++i)jobs.push_back(std::async(std::launch::async,[land]{for(int n=0;n<20;++n)if(!g::geometry_interiors_overlap(land,land))return false;return true;}));
    for(auto& job:jobs)check(job.get(),"independent thread-local geodesic solvers");
    using V=std::array<double,3>;
    auto vector=[](Point point)->V{const double r=std::acos(-1)/180e9,a=point.lat_ndeg*r,b=point.lon_ndeg*r;return {std::cos(a)*std::cos(b),std::cos(a)*std::sin(b),std::sin(a)};};
    auto cross=[](V a,V b)->V{return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
    auto dot=[](V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    std::mt19937_64 random(71125);auto sample=[&]()->Point{return {int64_t(random()%170000000001ULL)-85000000000LL,int64_t(random()%360000000001ULL)-180000000000LL};};
    for(int i=0;i<200;++i){Ring ring{sample(),sample(),sample()};if(!g::is_simple_polygon(ring))continue;
        V a=vector(ring[0]),b=vector(ring[1]),c=vector(ring[2]);std::array<V,3> ns{cross(a,b),cross(b,c),cross(c,a)};double sign=dot(ns[0],c)>0?1:-1;
        for(int j=0;j<100;++j){const auto point=sample();const auto dir=vector(point);bool clear=true,inside=true;
            for(auto n:ns){double side=dot(n,dir);clear=clear&&std::abs(side)>0.025*std::sqrt(dot(n,n));inside=inside&&side*sign>0;}
            if(clear)check((g::locate_point(ring,point)==g::PointLocation::inside)==inside,"independent global containment oracle away from edges");}}
}
void grid() {
    GeographicLib::Ellipsoid earth(GeographicLib::Constants::WGS84_a(),GeographicLib::Constants::WGS84_f());
    check(powgeo::kLatitudeCdf.front()==0 && powgeo::kLatitudeCdf.back()==4294967296ULL,"global CDF covers full ticket space");
    for(size_t i=1;i<powgeo::kLatitudeCdf.size();++i){check(powgeo::kLatitudeCdf[i]>powgeo::kLatitudeCdf[i-1],"every latitude band selectable");
        const double lat=-90+i/10.0;const auto expected=uint64_t(std::llround((1+GeographicLib::Math::sind(earth.AuthalicLatitude(lat)))*2147483648.0));
        check(std::abs(double(expected)-powgeo::kLatitudeCdf[i])<=1,"CDF proportional to pinned WGS84 ellipsoidal area");}
    bool north=false,south=false;geocoin::Hash256 hash{};
    for(uint32_t i=0;i<20000;++i){for(size_t b=0;b<hash.size();++b)hash[b]=static_cast<uint8_t>((i>>(b%4)*8)+b);
        const auto tag=powgeo::compute_geo_tag(i+1,hash);const auto decoded=powgeo::point_from_geo_tag(tag.geo_tag);
        check(decoded && decoded->point()==tag.point(),"point intrinsically recoverable without parent history");
        north=north||tag.lat_ndeg>85'000'000'000LL;south=south||tag.lat_ndeg<-85'000'000'000LL;}
    check(north&&south,"GeoPoW reaches both polar bands");
}
Json fingerprint(const std::string& platform) {
    geocoin::bytes bytes;auto u64=[&](uint64_t value){for(int i=0;i<8;++i)bytes.push_back(static_cast<uint8_t>(value>>(8*i)));};
    size_t cases=0;geocoin::Hash256 hash{};
    for(uint32_t i=0;i<10000;++i){for(size_t b=0;b<hash.size();++b)hash[b]=static_cast<uint8_t>((i>>(b%4)*8)+b);
        const auto value=powgeo::compute_geo_tag(i+1,hash);u64(value.geo_tag);u64(value.lat_ndeg);u64(value.lon_ndeg);++cases;}
    for(const auto& geometry:{square(0,0),square(48.123456789,8.987654321,0.00001),shape({p(2,1),p(1,2),p(3,3)}),shape({p(89,0),p(89,120),p(89,-120)})}){
        const g::PreparedGeometry prepared(geometry);u64(g::area_square_centimetres(geometry).value());
        for(const auto point:{p(0.05,0.05),p(-0.00009,0.05),p(90,0),p(-90,0),p(0,180)}){
            u64(static_cast<unsigned>(prepared.locate(point)));u64(static_cast<uint64_t>(std::llround(prepared.distance_m(point).value()*1000)));++cases;}
        geocoin::nft::NFTMetadata metadata{};metadata.geometry=geometry;const auto encoded=metadata.serialize();bytes.insert(bytes.end(),encoded.begin(),encoded.end());
    }
    return {{"platform",platform},{"rules_version",1},{"library_version",g::kGeodesyVersion},{"cases",cases},
        {"sha256",geocoin::utils::hash256_to_hex(geocoin::crypto::sha256(bytes))}};
}
}
int main(int argc,char** argv) {
    try {
        if(argc>=2 && std::string(argv[1])=="--fingerprint") {std::cout<<fingerprint(argc>=3?argv[2]:"local").dump()<<'\n';return 0;}
        distances();metrics_and_canonicalization();topology_and_complex_parts();geojson();geometry_limits_and_metrics();
        codec_diagnostics_and_rounding();global_and_rounding();grid();registry_and_pol();
        std::cout<<checks<<" geodesy v1 checks passed\n";return 0;
    } catch(const std::exception& error){std::fesetround(FE_TONEAREST);std::cerr<<"Geodesy test failed: "<<error.what()<<'\n';return 1;}
}
