#include "AnycubicSLA.hpp"
#include "GCode/ThumbnailData.hpp"
#include "SLA/RasterBase.hpp"
#include "libslic3r/SLAPrint.hpp"

#include <sstream>
#include <iostream>
#include <fstream>

#include <boost/algorithm/string/replace.hpp>
#include <boost/log/trivial.hpp>


#define TAG_INTRO   "ANYCUBIC\0\0\0\0"
#define TAG_HEADER  "HEADER\0\0\0\0\0\0"
#define TAG_PREVIEW "PREVIEW\0\0\0\0\0"
#define TAG_LAYERS  "LAYERDEF\0\0\0\0"
#define TAG_EXTRA   "EXTRA\0\0\0\0\0\0\0"
#define TAG_MACHINE "MACHINE\0\0\0\0\0"
#define TAG_MACHINE "MODEL\0\0\0\0\0\0\0"

#define CFG_LIFT_DISTANCE "LIFT_DISTANCE"
#define CFG_LIFT_SPEED "LIFT_SPEED"
#define CFG_RETRACT_SPEED "RETRACT_SPEED"
#define CFG_DELAY_BEFORE_EXPOSURE "DELAY_BEFORE_EXPOSURE"
#define CFG_BOTTOM_LIFT_SPEED "BOTTOM_LIFT_SPEED"
#define CFG_BOTTOM_LIFT_DISTANCE "BOTTOM_LIFT_DISTANCE"
#define CFG_ANTIALIASING "ANTIALIASING"

// Introduced with 515
#define CFG_EXTRA_LIFT_DISTANCE "EXTRA_LIFT_DISTANCE"
#define CFG_EXTRA_LIFT_SPEED "EXTRA_LIFT_SPEED"
#define CFG_EXTRA_RETRACT_SPEED "EXTRA_RETRACT_SPEED"

#define PREV_W 224
#define PREV_H 168
#define PREV_DPI 42

#define LAYER_SIZE_ESTIMATE (32 * 1024)

namespace Slic3r {

static void anycubicsla_get_pixel_span(const std::uint8_t* ptr, const std::uint8_t* end,
                               std::uint8_t& pixel, size_t& span_len)
{
    size_t max_len;

    span_len = 0;
    pixel = (*ptr) & 0xF0;
    // the maximum length of the span depends on the pixel color
    max_len = (pixel == 0 || pixel == 0xF0) ? 0xFFF : 0xF;
    while (ptr < end && span_len < max_len && ((*ptr) & 0xF0) == pixel) {
        span_len++;
        ptr++;
    }
}

struct AnycubicSLARasterEncoder
{
    sla::EncodedRaster operator()(const void *ptr,
                                  size_t      w,
                                  size_t      h,
                                  size_t      num_components)
    {
        std::vector<uint8_t> dst;
        size_t               span_len;
        std::uint8_t         pixel;
        auto                 size = w * h * num_components;
        dst.reserve(size);

        const std::uint8_t *src = reinterpret_cast<const std::uint8_t *>(ptr);
        const std::uint8_t *src_end = src + size;
        while (src < src_end) {
            anycubicsla_get_pixel_span(src, src_end, pixel, span_len);
            src += span_len;
            // fully transparent of fully opaque pixel
            if (pixel == 0 || pixel == 0xF0) {
                pixel = pixel | (span_len >> 8);
                std::copy(&pixel, (&pixel) + 1, std::back_inserter(dst));
                pixel = span_len & 0xFF;
                std::copy(&pixel, (&pixel) + 1, std::back_inserter(dst));
            }
            // antialiased pixel
            else {
                pixel = pixel | span_len;
                std::copy(&pixel, (&pixel) + 1, std::back_inserter(dst));
            }
        }

        return sla::EncodedRaster(std::move(dst), "pwimg");
    }
};

using ConfMap = std::map<std::string, std::string>;

typedef struct anycubicsla_format_intro
{
    char          tag[12];
    std::uint32_t version;  // value 1 (also known as 515, 516 and 517)
    std::uint32_t area_num; // Number of tables - usually 4
    std::uint32_t header_data_offset;
    std::uint32_t software_data_offset; // unused in version 1, 6357060 for 516 only needed for 517
    std::uint32_t preview_data_offset;
    std::uint32_t layer_color_offset; // unused in version 1
    std::uint32_t layer_data_offset;
    std::uint32_t extra_data_offset; // 516 onwards
    std::uint32_t machine_data_offset; //Does not exist in <516
    std::uint32_t image_data_offset;
    std::uint32_t software_data_offset; // 517
    std::uint32_t model_data_offset; // 517
    
} anycubicsla_format_intro;

typedef struct anycubicsla_format_header
{
    char          tag[12];
    std::uint32_t payload_size;
    std::float_t  pixel_size_um;
    std::float_t  layer_height_mm;
    std::float_t  exposure_time_s;
    std::float_t  delay_before_exposure_s;
    std::float_t  bottom_exposure_time_s;
    std::float_t  bottom_layer_count;
    std::float_t  lift_distance_mm;
    std::float_t  lift_speed_mms;
    std::float_t  retract_speed_mms;
    std::float_t  volume_ml;
    std::uint32_t antialiasing;
    std::uint32_t res_x;
    std::uint32_t res_y;
    std::float_t  weight_g;
    std::float_t  price;
    std::uint32_t price_currency;
    std::uint32_t per_layer_override; // ? unknown meaning ?
    std::uint32_t print_time_s;
    std::uint32_t transition_layer_count;
    std::uint32_t transition_layer_type; // usually 0

    std::uint32_t advanced_mode // 516 and onward

    std::uint16_t gray; //517 and onward
    std::uint16_t blue_level; //517 and onward
    std::uint32_t resin_code; // //517 and onward, 1579548560
} anycubicsla_format_header;

typedef struct anycubicsla_format_preview
{
    char          tag[12];
    std::uint32_t payload_size;
    std::uint32_t preview_w;
    std::uint32_t preview_dpi;
    std::uint32_t preview_h;
    // raw image data in BGR565 format
     std::uint8_t pixels[PREV_W * PREV_H * 2];
} anycubicsla_format_preview;

std::uint8_t grey_lookup = {
            15, 31, 47,    // 1,2,3
            63, 79, 95,    // 4,5,6
            111, 127, 143, // 7,8,9
            159, 175, 191, // 10,11,12
            207, 223, 239, // 13,14,15
            255  // 16
};

typedef struct anycubicsla_format_layers_colors
{
    std::uint32_t use_full_grayscale;
    std::uint32_t grey_max_count; //typically 16
    std::uint8_t grey[16];
    std::uint32_t unknown;
} anycubicsla_format_layers_colors;

typedef struct anycubicsla_format_layers_header
{
    char          tag[12];
    std::uint32_t payload_size;
    std::uint32_t layer_count;
} anycubicsla_format_layers_header;

typedef struct anycubicsla_format_layer
{
    std::uint32_t image_offset;
    std::uint32_t image_size;
    std::float_t  lift_distance_mm;
    std::float_t  lift_speed_mms;
    std::float_t  exposure_time_s;
    std::float_t  layer_height_mm;
    std::float_t  layer44; // unkown - usually 0
    std::float_t  layer48; // unkown - usually 0
} anycubicsla_format_layer;

typedef struct anycubicsla_format_misc
{
    std::float_t bottom_layer_height_mm;
    std::float_t bottom_lift_distance_mm;
    std::float_t bottom_lift_speed_mms;

} anycubicsla_format_misc;


typedef struct anycubicsla_format_extra
{
    char          tag[12];
    std::uint32_t payload_length; 
    std::uint32_t bottom_state_num; // 2
    std::float_t  lift_distance1_mm;
    std::float_t  lift_speed1_mms;
    std::float_t  retract_speed1_mms;
    std::float_t  lift_distance2_mm;
    std::float_t  lift_speed2_mms;
    std::float_t  retract_speed2_mms;
    std::uint32_t state_num; // 2
    std::float_t  lift_distance3_mm;
    std::float_t  lift_speed3_mms;
    std::float_t  retract_speed3_mms;
    std::float_t  lift_distance4_mm;
    std::float_t  lift_speed4_mms;
    std::float_t  retract_speed4_mms;
} anycubicsla_format_extra;

typedef struct anycubicsla_format_machine
{
    char          tag[12];
    std::uint32_t payload_size;
    char          name[96];
    char          image_format[24];
    std::float_t  volume_x;
    std::float_t  volume_y;
    std::float_t  volume_z;
    std::uint32_t version;
    std::uint32_t machine140;
} anycubicsla_format_machine;

typedef struct anycubicsla_format_software
{
    char          name[32]; // ANYCUBIC-PC
    std::uint32_t payload_size; // 164
    char          version[32];
    char          operating_system[64]; // win-x64
    char          opengl_version[32];// 3.3-CoreProfile
} anycubicsla_format_software;

typedef struct anycubicsla_format_model
{
    char          tag[12]; // MODEL
    std::uint32_t payload_size;
    
    std::float minX; 
    std::float minY;
    std::float minZ;
    std::float maxX;
    std::float maxY;
    std::float maxZ;
    std::uint32_t supports_enabled;
    std::float supports_density;
} anycubicsla_format_model;

class AnycubicSLAFormatConfigDef : public ConfigDef
{
public:
    AnycubicSLAFormatConfigDef()
    {
        add(CFG_LIFT_DISTANCE, coFloat);
        add(CFG_LIFT_SPEED, coFloat);
        add(CFG_RETRACT_SPEED, coFloat);
        add(CFG_DELAY_BEFORE_EXPOSURE, coFloat);
        add(CFG_BOTTOM_LIFT_DISTANCE, coFloat);
        add(CFG_BOTTOM_LIFT_SPEED, coFloat);
        add(CFG_ANTIALIASING, coInt);
    }
};

class AnycubicSLAFormatDynamicConfig : public DynamicConfig
{
public:
    AnycubicSLAFormatDynamicConfig(){};
    const ConfigDef *def() const override { return &config_def; }

private:
    AnycubicSLAFormatConfigDef config_def;
};

namespace {

std::float_t get_cfg_value_f(const DynamicConfig &cfg,
                             const std::string   &key,
                             const std::float_t  &def = 0.f)
{
    if (cfg.has(key)) {
        if (auto opt = cfg.option(key))
            return opt->getFloat();
    }

    return def;
}

int get_cfg_value_i(const DynamicConfig &cfg,
                    const std::string   &key,
                    const int           &def = 0)
{
    if (cfg.has(key)) {
        if (auto opt = cfg.option(key))
            return opt->getInt();
    }

    return def;
}

template<class T> void crop_value(T &val, T val_min, T val_max)
{
    if (val < val_min) {
        val = val_min;
    } else if (val > val_max) {
        val = val_max;
    }
}

void fill_preview(anycubicsla_format_preview &p,
                  anycubicsla_format_misc   &/*m*/,
                  const ThumbnailsList &thumbnails)
{

    p.preview_w    = PREV_W;
    p.preview_h    = PREV_H;
    p.preview_dpi  = PREV_DPI;
    p.payload_size = sizeof(p) - sizeof(p.tag) - sizeof(p.payload_size);
                     
    std::memset(p.pixels, 0 , sizeof(p.pixels));
    if (!thumbnails.empty()) {
        std::uint32_t dst_index;
        std::uint32_t i = 0;
        size_t len;
        size_t pixel_x = 0;
        auto t = thumbnails[0]; //use the first thumbnail
        len = t.pixels.size();
        //sanity check        
        if (len != PREV_W * PREV_H * 4)  {
            printf("incorrect thumbnail size. expected %ix%i\n", PREV_W, PREV_H);
            return;
        }
        // rearange pixels: they seem to be stored from bottom to top.
        dst_index = (PREV_W * (PREV_H - 1) * 2);
        while (i < len) {
            std::uint32_t pixel;
            std::uint32_t r = t.pixels[i++];
            std::uint32_t g = t.pixels[i++];
            std::uint32_t b = t.pixels[i++];
            i++; // Alpha
            // convert to BGRA565
            pixel = ((b >> 3) << 11) | ((g >>2) << 5) | (r >> 3);
            p.pixels[dst_index++] = pixel & 0xFF;
            p.pixels[dst_index++] = (pixel >> 8) & 0xFF;
            pixel_x++;
            if (pixel_x == PREV_W) {
                pixel_x = 0;
                dst_index -= (PREV_W * 4);
            }
        }
    }
}

void fill_header(anycubicsla_format_header &h,
                 anycubicsla_format_misc   &m,
                 const SLAPrint     &print,
                 std::uint32_t       layer_count)
{
    CNumericLocalesSetter locales_setter;

    std::float_t bottle_weight_g;
    std::float_t bottle_volume_ml;
    std::float_t bottle_cost;
    std::float_t material_density;
    auto        &cfg     = print.full_print_config();
    auto         mat_opt = cfg.option("material_notes");
    std::string  mnotes  = mat_opt? cfg.option("material_notes")->serialize() : "";
    // create a config parser from the material notes
    Slic3r::AnycubicSLAFormatDynamicConfig mat_cfg;
    SLAPrintStatistics              stats = print.print_statistics();

    // sanitize the string config
    boost::replace_all(mnotes, "\\n", "\n");
    boost::replace_all(mnotes, "\\r", "\r");
    mat_cfg.load_from_ini_string(mnotes,
                                 ForwardCompatibilitySubstitutionRule::Enable);

    h.layer_height_mm        = get_cfg_value_f(cfg, "layer_height");
    m.bottom_layer_height_mm = get_cfg_value_f(cfg, "initial_layer_height");
    h.exposure_time_s        = get_cfg_value_f(cfg, "exposure_time");
    h.bottom_exposure_time_s = get_cfg_value_f(cfg, "initial_exposure_time");
    h.bottom_layer_count =     get_cfg_value_i(cfg, "faded_layers");
    if (layer_count < h.bottom_layer_count) {
        h.bottom_layer_count = layer_count;
    }
    h.res_x     = get_cfg_value_i(cfg, "display_pixels_x");
    h.res_y     = get_cfg_value_i(cfg, "display_pixels_y");
    auto         dispo_opt = cfg.option("display_orientation");
    std::string  dispo  = dispo_opt? cfg.option("display_orientation")->serialize() : "landscape";
    if (dispo == "portrait") {
        std::swap(h.res_x, h.res_y);
    }

    bottle_weight_g = get_cfg_value_f(cfg, "bottle_weight") * 1000.0f;
    bottle_volume_ml = get_cfg_value_f(cfg, "bottle_volume");
    bottle_cost = get_cfg_value_f(cfg, "bottle_cost");
    material_density = bottle_weight_g / bottle_volume_ml;

    h.volume_ml = (stats.objects_used_material + stats.support_used_material) / 1000;
    h.weight_g           = h.volume_ml * material_density;
    h.price              = (h.volume_ml * bottle_cost) /  bottle_volume_ml;
    h.price_currency     = '$';
    h.antialiasing       = 1;
    h.per_layer_override = 0;

    // TODO - expose these variables to the UI rather than using material notes
    if (mat_cfg.has(CFG_ANTIALIASING)) {
        h.antialiasing = get_cfg_value_i(mat_cfg, CFG_ANTIALIASING);
        crop_value(h.antialiasing, (uint32_t) 0, (uint32_t) 1);
    } else {
        h.antialiasing = 1;
    }

    h.delay_before_exposure_s = get_cfg_value_f(mat_cfg, CFG_DELAY_BEFORE_EXPOSURE, 0.5f);
    crop_value(h.delay_before_exposure_s, 0.0f, 1000.0f);

    h.lift_distance_mm = get_cfg_value_f(mat_cfg, CFG_LIFT_DISTANCE, 8.0f);
    crop_value(h.lift_distance_mm, 0.0f, 100.0f);

    if (mat_cfg.has(CFG_BOTTOM_LIFT_DISTANCE)) {
        m.bottom_lift_distance_mm = get_cfg_value_f(mat_cfg,
                                                    CFG_BOTTOM_LIFT_DISTANCE,
                                                    8.0f);
        crop_value(h.lift_distance_mm, 0.0f, 100.0f);
    } else {
        m.bottom_lift_distance_mm = h.lift_distance_mm;
    }

    h.lift_speed_mms = get_cfg_value_f(mat_cfg, CFG_LIFT_SPEED, 2.0f);
    crop_value(m.bottom_lift_speed_mms, 0.1f, 20.0f);

    if (mat_cfg.has(CFG_BOTTOM_LIFT_SPEED)) {
        m.bottom_lift_speed_mms = get_cfg_value_f(mat_cfg, CFG_BOTTOM_LIFT_SPEED, 2.0f);
        crop_value(m.bottom_lift_speed_mms, 0.1f, 20.0f);
    } else {
        m.bottom_lift_speed_mms = h.lift_speed_mms;
    }

    h.retract_speed_mms = get_cfg_value_f(mat_cfg, CFG_RETRACT_SPEED, 3.0f);
    crop_value(h.lift_speed_mms, 0.1f, 20.0f);

    h.print_time_s = (h.bottom_layer_count * h.bottom_exposure_time_s) +
                     ((layer_count - h.bottom_layer_count) *
                      h.exposure_time_s) +
                     (layer_count * h.lift_distance_mm / h.retract_speed_mms) +
                     (layer_count * h.lift_distance_mm / h.lift_speed_mms) +
                     (layer_count * h.delay_before_exposure_s);


    std::float_t display_w = get_cfg_value_f(cfg, "display_width", 100) * 1000;
    if (dispo == "portrait") {
        h.pixel_size_um = (int)((display_w / h.res_y) + 0.5f);
    } else {
        h.pixel_size_um = (int)((display_w / h.res_x) + 0.5f);
    }
    
    h.payload_size  = sizeof(h) - sizeof(h.tag) - sizeof(h.payload_size);

    if (version < ANYCUBIC_SLA_VERSION_516) {
        h.payload_size -= sizeof(h.advance_mode);
    }
    if (version < ANYCUBIC_SLA_VERSION_517){
        h.payload_size -= sizeof(h.gray);
        h.payload_size -= sizeof(h.blur_level);
        h.payload_size -= sizeof(h.resin_code);
    }
}

void fill_color(anycubicsla_format_layers_color &color,){
    color.use_full_grayscale = 0;
    color.grey_max_count = 16;
    for (int i = 0; i < color.grey_max_count; i++) {
        color.grey[i] = grey_lookup[i];
    }
    color.unknown = 0
}


void fill_extra(anycubicsla_format_extra  &e,
                const SLAPrint     &print)
{
    auto        &cfg     = print.full_print_config();
    auto         mat_opt = cfg.option("material_notes");
    std::string  mnotes  = mat_opt? cfg.option("material_notes")->serialize() : "";
    // create a config parser from the material notes
    Slic3r::PwmxFormatDynamicConfig mat_cfg;

    // sanitize the string config
    boost::replace_all(mnotes, "\\n", "\n");
    boost::replace_all(mnotes, "\\r", "\r");
    mat_cfg.load_from_ini_string(mnotes,
                                 ForwardCompatibilitySubstitutionRule::Enable);

    // unknown fields - the values from TEST.pwma are used
    e.extra0 = 24;
    e.extra4 = 2;
    e.extra32 = 2;

    // Currently it is unknown when (during printing) these values are applied
    // and which values (layer section or extra section) have higher priority.
    // These configurtion options can be set in material notes.

    e.lift_distance1_mm  = get_cfg_value_f(mat_cfg, CFG_EXTRA_LIFT_DISTANCE "1", 1.5f);;
    e.lift_speed1_mms    = get_cfg_value_f(mat_cfg, CFG_EXTRA_LIFT_SPEED "1", 2.0f);;
    e.retract_speed1_mms = get_cfg_value_f(mat_cfg, CFG_EXTRA_RETRACT_SPEED "1", 3.0f);;

    e.lift_distance2_mm  = get_cfg_value_f(mat_cfg, CFG_EXTRA_LIFT_DISTANCE "2", 4.5f);;
    e.lift_speed2_mms    = get_cfg_value_f(mat_cfg, CFG_EXTRA_LIFT_SPEED "2", 4.0f);;
    e.retract_speed2_mms = get_cfg_value_f(mat_cfg, CFG_EXTRA_RETRACT_SPEED "2", 6.0f);;

    e.lift_distance3_mm  = get_cfg_value_f(mat_cfg, CFG_EXTRA_LIFT_DISTANCE "3", 1.5f);;
    e.lift_speed3_mms    = get_cfg_value_f(mat_cfg, CFG_EXTRA_LIFT_SPEED "3", 2.0f);;
    e.retract_speed3_mms = get_cfg_value_f(mat_cfg, CFG_EXTRA_RETRACT_SPEED "3", 3.0f);;

    e.lift_distance4_mm  = get_cfg_value_f(mat_cfg, CFG_EXTRA_LIFT_DISTANCE "4", 4.0f);;
    e.lift_speed4_mms    = get_cfg_value_f(mat_cfg, CFG_EXTRA_LIFT_SPEED "4", 2.0f);;
    e.retract_speed4_mms = get_cfg_value_f(mat_cfg, CFG_EXTRA_RETRACT_SPEED "4", 3.0f);;

    // ensure sane values are set
    crop_value(e.lift_distance1_mm, 0.1f, 100.0f);
    crop_value(e.lift_distance2_mm, 0.1f, 100.0f);
    crop_value(e.lift_distance3_mm, 0.1f, 100.0f);
    crop_value(e.lift_distance4_mm, 0.1f, 100.0f);

    crop_value(e.lift_speed1_mms, 0.1f, 20.0f);
    crop_value(e.lift_speed2_mms, 0.1f, 20.0f);
    crop_value(e.lift_speed3_mms, 0.1f, 20.0f);
    crop_value(e.lift_speed4_mms, 0.1f, 20.0f);

    crop_value(e.retract_speed1_mms, 0.1f, 20.0f);
    crop_value(e.retract_speed2_mms, 0.1f, 20.0f);
    crop_value(e.retract_speed3_mms, 0.1f, 20.0f);
    crop_value(e.retract_speed4_mms, 0.1f, 20.0f);
}

void fill_machine(anycubicsla_format_machine &m,
                  const SLAPrint      &print,
                  int                 version)
{
    auto        &cfg     = print.full_print_config();
    auto         mat_opt = cfg.option("material_notes");
    std::string  mnotes  = mat_opt? cfg.option("material_notes")->serialize() : "";
    // create a config parser from the material notes
    Slic3r::PwmxFormatDynamicConfig mat_cfg;
    auto        print_opt = cfg.option("printer_notes");
    std::string pnotes    = print_opt? cfg.option("printer_notes")->serialize() : "";
    // create a vector of strings from the printer notes
    std::vector<std::string> pnotes_items;

    // sanitize the  material notes
    boost::replace_all(mnotes, "\\n", "\n");
    boost::replace_all(mnotes, "\\r", "\r");
    mat_cfg.load_from_ini_string(mnotes,
                                 ForwardCompatibilitySubstitutionRule::Enable);

    // sanitize the printer notes
    boost::replace_all(pnotes, "\\n", "\n");
    boost::replace_all(pnotes, "\\r", "\r");
    boost::split(pnotes_items, pnotes, boost::is_any_of("\n\r"), boost::token_compress_on);

    std::string name = get_vec_value_s(pnotes_items, CFG_EXPORT_MACHINE_NAME, "Photon Mono");
    std::strncpy((char*) m.name, name.c_str(), sizeof(m.name));
    std::strncpy((char*) m.image_format, "pw0Img", sizeof(m.image_format));

    m.volume_x = get_cfg_value_f(cfg, "display_width");
    m.volume_y = get_cfg_value_f(cfg, "display_height");
    m.volume_z = get_cfg_value_f(cfg, "max_print_height", 160);
    m.version  = version;
    m.machine140 = 0x634701; // unknown purpose (found in  TEST.pwma  - Photon Mono 4K)
    m.payload_size = sizeof(m);

    auto dispo_opt = cfg.option("display_orientation");
    std::string dispo = dispo_opt? cfg.option("display_orientation")->serialize() : "landscape";
    if (dispo == "portrait") {
        std::swap(m.volume_x, m.volume_y);
    }


void fill_software(anycubicsla_format_software &s)
{
    strcpy(&s.name, "PRUSASLICER");
    s.payload_size = 164;
    s.version = 0;
    strcpy(&s.opengl_version, "3.3-CoreProfile");
}      

void fill_model(anycubicsla_format_model &m)
{
    /** Taken from UVTools
    var rect = slicerFile.BoundingRectangleMillimeters;
    m.maxX = (float)Math.Round(rect.Width / 2, 3);
    m.minX = -m.maxX;

    m.maxY = (float)Math.Round(rect.Height / 2, 3);
    m.minY = -m.maxY;

    m.minZ = 0;
    m.maxZ = slicerFile.PrintHeight;
    */
    m.payload_size = sizeof(m);
}      

}// namespace

std::unique_ptr<sla::RasterBase> AnycubicSLAArchive::create_raster() const
{
    sla::Resolution     res;
    sla::PixelDim       pxdim;
    std::array<bool, 2> mirror;

    double w  = m_cfg.display_width.getFloat();
    double h  = m_cfg.display_height.getFloat();
    auto   pw = size_t(m_cfg.display_pixels_x.getInt());
    auto   ph = size_t(m_cfg.display_pixels_y.getInt());

    mirror[X] = m_cfg.display_mirror_x.getBool();
    mirror[Y] = m_cfg.display_mirror_y.getBool();

    auto                         ro = m_cfg.display_orientation.getInt();
    sla::RasterBase::Orientation orientation =
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait :
                                            sla::RasterBase::roLandscape;

    if (orientation == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    res   = sla::Resolution{pw, ph};
    pxdim = sla::PixelDim{w / pw, h / ph};
    sla::RasterBase::Trafo tr{orientation, mirror};

    double gamma = m_cfg.gamma_correction.getFloat();

    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

sla::RasterEncoder AnycubicSLAArchive::get_encoder() const
{
    return AnycubicSLARasterEncoder{};
}
static void anycubicsla_write_int16(std::ofstream &out, std::uint32_t val)
{
    const char i1 = (val & 0xFF);
    const char i2 = (val >> 8) & 0xFF;

    out.write((const char *) &i1, 1);
    out.write((const char *) &i2, 1);
}
// Endian safe write of little endian 32bit ints
static void anycubicsla_write_int32(std::ofstream &out, std::uint32_t val)
{
    const char i1 = (val & 0xFF);
    const char i2 = (val >> 8) & 0xFF;
    const char i3 = (val >> 16) & 0xFF;
    const char i4 = (val >> 24) & 0xFF;

    out.write((const char *) &i1, 1);
    out.write((const char *) &i2, 1);
    out.write((const char *) &i3, 1);
    out.write((const char *) &i4, 1);
}
static void anycubicsla_write_float(std::ofstream &out, std::float_t val)
{
    std::uint32_t *f = (std::uint32_t *) &val;
    anycubicsla_write_int32(out, *f);
}

static void anycubicsla_write_intro(std::ofstream &out, anycubicsla_format_intro &i, uint32_t version)
{
    out.write(TAG_INTRO, sizeof(i.tag));
    anycubicsla_write_int32(out, i.version);
    anycubicsla_write_int32(out, i.area_num);
    anycubicsla_write_int32(out, i.header_data_offset);
    anycubicsla_write_int32(out, i.software_data_offset);
    anycubicsla_write_int32(out, i.preview_data_offset);
    anycubicsla_write_int32(out, i.layer_color_offset);
    anycubicsla_write_int32(out, i.layer_data_offset);
    anycubicsla_write_int32(out, i.extra_data_offset); //515
    if (version >= ANYCUBIC_SLA_VERSION_516) {
        anycubicsla_write_int32(out, i.machine_data_offset); //516
    }
    if (version >= ANYCUBIC_SLA_VERSION_517) {
        anycubicsla_write_int32(out, i.model_data_offset); //517
    }
    anycubicsla_write_int32(out, i.image_data_offset);
}

static void anycubicsla_write_header(std::ofstream &out, anycubicsla_format_header &h, uint32_t version)
{
    out.write(TAG_HEADER, sizeof(h.tag));
    anycubicsla_write_int32(out, h.payload_size);
    anycubicsla_write_float(out, h.pixel_size_um);
    anycubicsla_write_float(out, h.layer_height_mm);
    anycubicsla_write_float(out, h.exposure_time_s);
    anycubicsla_write_float(out, h.delay_before_exposure_s);
    anycubicsla_write_float(out, h.bottom_exposure_time_s);
    anycubicsla_write_float(out, h.bottom_layer_count);
    anycubicsla_write_float(out, h.lift_distance_mm);
    anycubicsla_write_float(out, h.lift_speed_mms);
    anycubicsla_write_float(out, h.retract_speed_mms);
    anycubicsla_write_float(out, h.volume_ml);
    anycubicsla_write_int32(out, h.antialiasing);
    anycubicsla_write_int32(out, h.res_x);
    anycubicsla_write_int32(out, h.res_y);
    anycubicsla_write_float(out, h.weight_g);
    anycubicsla_write_float(out, h.price);
    anycubicsla_write_int32(out, h.price_currency);
    anycubicsla_write_int32(out, h.per_layer_override);
    anycubicsla_write_int32(out, h.print_time_s);
    anycubicsla_write_int32(out, h.transition_layer_count);
    anycubicsla_write_int32(out, h.transition_layer_type);
    if (version >= ANYCUBIC_SLA_VERSION_516) {
        anycubicsla_write_int32(out, h.advance_mode);
    }
    if (version >= ANYCUBIC_SLA_VERSION_517){
        anycubicsla_write_int16(out, h.gray);
        anycubicsla_write_int16(out, h.blur_level);
        anycubicsla_write_int16(out, h.resin_code);
    }
}

static void anycubicsla_write_preview(std::ofstream &out, anycubicsla_format_preview &p)
{
    out.write(TAG_PREVIEW, sizeof(p.tag));
    anycubicsla_write_int32(out, p.payload_size);
    anycubicsla_write_int32(out, p.preview_w);
    anycubicsla_write_int32(out, p.preview_dpi);
    anycubicsla_write_int32(out, p.preview_h);
    out.write((const char*) p.pixels, sizeof(p.pixels));
}

static void anycubicsla_write_layer_color(std::ofstream &out, anycubicsla_format_layers_color &c)
{
    anycubicsla_write_int32(out, c.use_full_grayscale);
    anycubicsla_write_int32(out, c.grey_max_count);
    for (int i=0; i< c.grey_max_count; i++) {
        out.write((const char *) &c.grey[i], 1);
    }
    anycubicsla_write_int32(out, c.preview_dpi);
    anycubicsla_write_int32(out, c.unknown);
}

static void anycubicsla_write_extra(std::ofstream &out, anycubicsla_format_extra &e)
{
    out.write(TAG_EXTRA, sizeof(e.tag));
    anycubicsla_write_int32(out, e.extra0);

    anycubicsla_write_int32(out, e.extra4);
    anycubicsla_write_float(out, e.lift_distance1_mm);
    anycubicsla_write_float(out, e.lift_speed1_mms);
    anycubicsla_write_float(out, e.retract_speed1_mms);
    anycubicsla_write_float(out, e.lift_distance2_mm);
    anycubicsla_write_float(out, e.lift_speed2_mms);
    anycubicsla_write_float(out, e.retract_speed2_mms);

    anycubicsla_write_int32(out, e.extra32);
    anycubicsla_write_float(out, e.lift_distance3_mm);
    anycubicsla_write_float(out, e.lift_speed3_mms);
    anycubicsla_write_float(out, e.retract_speed3_mms);
    anycubicsla_write_float(out, e.lift_distance4_mm);
    anycubicsla_write_float(out, e.lift_speed4_mms);
    anycubicsla_write_float(out, e.retract_speed4_mms);
}

static void anycubicsla_write_machine(std::ofstream &out, anycubicsla_format_machine &m)
{
    out.write(TAG_MACHINE, sizeof(m.tag));
    anycubicsla_write_int32(out, m.payload_size);

    out.write(m.name, sizeof(m.name));
    out.write(m.image_format, sizeof(m.image_format));
    anycubicsla_write_float(out, m.volume_x);
    anycubicsla_write_float(out, m.volume_y);
    anycubicsla_write_float(out, m.volume_z);
    anycubicsla_write_int32(out, m.version);
    anycubicsla_write_int32(out, m.machine140);
}

static void anycubicsla_write_model(std::ofstream &out, anycubicsla_format_model &m)
{
    out.write(TAG_MODEL, sizeof(m.tag));
    anycubicsla_write_int32(out, m.payload_size);

    anycubicsla_write_float(out, m.minX);
    anycubicsla_write_float(out, m.minY);
    anycubicsla_write_float(out, m.minZ);
    anycubicsla_write_float(out, m.maxX);
    anycubicsla_write_float(out, m.maxY);
    anycubicsla_write_float(out, m.maxZ);
    anycubicsla_write_int32(out, m.supports_enabled);
    anycubicsla_write_float(out, m.supports_density);
}

static void anycubicsla_write_software(std::ofstream &out, anycubicsla_format_software &s)
{
    out.write(m.name, sizeof(m.name));
    anycubicsla_write_int32(out, m.payload_size);
    out.write(m.version, sizeof(m.version));
    out.write(m.operating_system, sizeof(m.operating_system));
    out.write(m.opengl_version, sizeof(m.opengl_version));
}

static void anycubicsla_write_layers_header(std::ofstream &out, anycubicsla_format_layers_header &h)
{
    out.write(TAG_LAYERS, sizeof(h.tag));
    anycubicsla_write_int32(out, h.payload_size);
    anycubicsla_write_int32(out, h.layer_count);
}

static void anycubicsla_write_layer(std::ofstream &out, anycubicsla_format_layer &l)
{
    anycubicsla_write_int32(out, l.image_offset);
    anycubicsla_write_int32(out, l.image_size);
    anycubicsla_write_float(out, l.lift_distance_mm);
    anycubicsla_write_float(out, l.lift_speed_mms);
    anycubicsla_write_float(out, l.exposure_time_s);
    anycubicsla_write_float(out, l.layer_height_mm);
    anycubicsla_write_float(out, l.layer44);
    anycubicsla_write_float(out, l.layer48);
}

void AnycubicSLAArchive::export_print(const std::string     fname,
                               const SLAPrint       &print,
                               const ThumbnailsList &thumbnails,
                               const std::string    &/*projectname*/)
{
    std::uint32_t layer_count = m_layers.size();

    anycubicsla_format_intro         intro = {};
    anycubicsla_format_header        header = {};
    anycubicsla_format_preview       preview = {};
    anycubicsla_format_layers_header layers_header = {};
    anycubicsla_format_layers_color    color = {};
    anycubicsla_format_misc          misc = {};
    anycubicsla_format_extra         extra = {};
    anycubicsla_format_machine       machine = {};

    std::vector<uint8_t>      layer_images;
    std::uint32_t             image_offset;

    assert(m_version <= ANYCUBIC_SLA_FORMAT_VERSION_516);


    intro.version             = m_version;
    intro.area_num            = 4;
    switch m_version {
        case ANYCUBIC_SLA_FORMAT_VERSION_1:
            intro.area_num            = 4;
            break;
        case ANYCUBIC_SLA_FORMAT_VERSION_515:
            intro.area_num            =  5;
            break;
        case ANYCUBIC_SLA_FORMAT_VERSION_516:
            intro.area_num            = 8;
            break;
        case NYCUBIC_SLA_FORMAT_VERSION_517:
            intro.area_num            =  9;
            break;
    }

    intro.header_data_offset  = sizeof(intro);

    if (version <= ANYCUBIC_SLA_FORMAT_VERSION_515) {
        // v1 and v515 dont use machine data offset - reduce size by 4 
        intro.header_data_offset -= sizeof(intro.machine_data_offset)
    } else  if (version <= ANYCUBIC_SLA_FORMAT_VERSION_516) {
        // <517 dont use model data offset - reduce size by 4 
        intro.header_data_offset -= sizeof(intro.model_data_offset);
    }

    intro.preview_data_offset = intro.header_data_offset + sizeof(header);

    fill_header(header, misc, print, layer_count);
    fill_preview(preview, misc, thumbnails);

    // 515 introduced grayscale lookup table
    if version >= ANYCUBIC_SLA_FORMAT_VERSION_515 {
        //Fill greyscale lookup, dont use it, required from 516 onward
        fill_color(color);

        //v1 calculates the preview payload size incorrectly, fixed with 515
        preview.payload_size = sizeof(p);

        intro.layer_color_offset = intro.preview_data_offset + sizeof(preview);
        intro.layer_data_offset  = intro.layer_color_offset + sizeof(color);
    } else {
        intro.layer_data_offset  = intro.preview_data_offset + sizeof(preview);
    }
    // Image data following the layers
    intro.image_data_offset = intro.layer_data_offset +
                    sizeof(layers_header) +
                    (sizeof(anycubicsla_format_layer) * layer_count);

    // Introduced with 516: Extra settings and machine settings
    if version >= ANYCUBIC_SLA_FORMAT_VERSION_516) {
        fill_extra(extra, print);
        fill_machine(machine, print, intro.version);

        // Extra is following the layers but before image data
        intro.extra_data_offset = intro.image_data_offset;
        intro.machine_data_offset = intro.extra_data_offset +  sizeof(extra);
        intro.image_data_offset = intro.machine_data_offset + sizeof(machine);
    } else {

    }

    // Introduced with 517: Software definition and model definition
    if version >= ANYCUBIC_SLA_FORMAT_VERSION_517) {
        // FIXME
        fill_software(software);
        fill_model(model);

        // software / model is following the extra but before image data
        intro.software_data_offset = intro.image_data_offset;
        intro.model_data_offset = intro.software_data_offset + sizeof(software);
        intro.image_data_offset = intro.model_data_offset + sizeof(model);
    }

    try {
        // open the file and write the contents
        std::ofstream out;
        out.open(fname, std::ios::binary | std::ios::out | std::ios::trunc);
        anycubicsla_write_intro(out, intro);
        anycubicsla_write_header(out, header, m_version);
        anycubicsla_write_preview(out, preview);
        if (intro.layer_color_offset != 0) {
            anycubicsla_write_layer_color(out, color);
        }

        layers_header.payload_size = intro.image_data_offset - intro.layer_data_offset -
                        sizeof(layers_header.tag)  - sizeof(layers_header.payload_size);
        layers_header.layer_count = layer_count;
        anycubicsla_write_layers_header(out, layers_header);

        //layers
        layer_images.reserve(layer_count * LAYER_SIZE_ESTIMATE);
        image_offset = intro.image_data_offset;
        size_t i = 0;
        for (const sla::EncodedRaster &rst : m_layers) {
            anycubicsla_format_layer l;
            std::memset(&l, 0, sizeof(l));
            l.image_offset = image_offset;
            l.image_size = rst.size();
            if (i < header.bottom_layer_count) {
                l.exposure_time_s = header.bottom_exposure_time_s;
                l.layer_height_mm = misc.bottom_layer_height_mm;
                l.lift_distance_mm = misc.bottom_lift_distance_mm;
                l.lift_speed_mms = misc.bottom_lift_speed_mms;
            } else {
                l.exposure_time_s = header.exposure_time_s;
                l.layer_height_mm = header.layer_height_mm;
                l.lift_distance_mm = header.lift_distance_mm;
                l.lift_speed_mms = header.lift_speed_mms;
            }
            image_offset += l.image_size;
            anycubicsla_write_layer(out, l);
            // add the rle encoded layer image into the buffer
            const char* img_start = reinterpret_cast<const char*>(rst.data());
            const char* img_end = img_start + rst.size();
            std::copy(img_start, img_end, std::back_inserter(layer_images));
            i++;
        }
        if (intro.extra_data_offset != 0) {
            anycubicsla_write_extra(out, extra);
        }
        if (intro.machine_data_offset != 0) {
            anycubicsla_write_machine(out, machine);
        }
        if (intro.software_data_offset != 0) {
            anycubicsla_write_software(out, software);
        }
        if (intro.model_data_offset != 0) {
            anycubicsla_write_model(out, model);
        }
        const char* img_buffer = reinterpret_cast<const char*>(layer_images.data());
        out.write(img_buffer, layer_images.size());
        out.close();
    } catch(std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << e.what();
        // Rethrow the exception
        throw;
    }

}

} // namespace Slic3r
