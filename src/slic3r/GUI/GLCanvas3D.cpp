#include "libslic3r/libslic3r.h"
#include "GLCanvas3D.hpp"

#include "admesh/stl.h"
#include "polypartition.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/PreviewData.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Technologies.hpp"
#include "libslic3r/Tesselate.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/PresetBundle.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/GUI_Preview.hpp"
#if ENABLE_NON_STATIC_CANVAS_MANAGER
#include "slic3r/GUI/GLCanvas3DManager.hpp"
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
#include "slic3r/GUI/3DBed.hpp"
#include "slic3r/GUI/Camera.hpp"

#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_ObjectManipulation.hpp"
#include "Mouse3DController.hpp"
#include "I18N.hpp"

#if ENABLE_RETINA_GL
#include "slic3r/Utils/RetinaHelper.hpp"
#endif

#include <GL/glew.h>

#include <wx/glcanvas.h>
#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/tooltip.h>
#include <wx/debug.h>
#include <wx/fontutil.h>

// Print now includes tbb, and tbb includes Windows. This breaks compilation of wxWidgets if included before wx.
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"

#include "wxExtensions.hpp"

#include <tbb/parallel_for.h>
#include <tbb/spin_mutex.h>

#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <iostream>
#include <float.h>
#include <algorithm>
#include <cmath>
#include "DoubleSlider.hpp"
#if !ENABLE_CANVAS_TOOLTIP_USING_IMGUI
#if ENABLE_RENDER_STATISTICS
#include <chrono>
#endif // ENABLE_RENDER_STATISTICS
#endif // !ENABLE_CANVAS_TOOLTIP_USING_IMGUI

#include <imgui/imgui_internal.h>

static const float TRACKBALLSIZE = 0.8f;

static const float DEFAULT_BG_DARK_COLOR[3] = { 0.478f, 0.478f, 0.478f };
static const float DEFAULT_BG_LIGHT_COLOR[3] = { 0.753f, 0.753f, 0.753f };
static const float ERROR_BG_DARK_COLOR[3] = { 0.478f, 0.192f, 0.039f };
static const float ERROR_BG_LIGHT_COLOR[3] = { 0.753f, 0.192f, 0.039f };
//static const float AXES_COLOR[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

// Number of floats
static const size_t MAX_VERTEX_BUFFER_SIZE     = 131072 * 6; // 3.15MB
// Reserve size in number of floats.
static const size_t VERTEX_BUFFER_RESERVE_SIZE = 131072 * 2; // 1.05MB
// Reserve size in number of floats, maximum sum of all preallocated buffers.
static const size_t VERTEX_BUFFER_RESERVE_SIZE_SUM_MAX = 1024 * 1024 * 128 / 4; // 128MB

namespace Slic3r {
namespace GUI {

Size::Size()
    : m_width(0)
    , m_height(0)
{
}

Size::Size(int width, int height, float scale_factor)
    : m_width(width)
    , m_height(height)
    , m_scale_factor(scale_factor)
{
}

int Size::get_width() const
{
    return m_width;
}

void Size::set_width(int width)
{
    m_width = width;
}

int Size::get_height() const
{
    return m_height;
}

void Size::set_height(int height)
{
    m_height = height;
}

int Size::get_scale_factor() const
{
    return m_scale_factor;
}

void Size::set_scale_factor(int scale_factor)
{
    m_scale_factor = scale_factor;
}

GLCanvas3D::LayersEditing::LayersEditing()
    : m_enabled(false)
    , m_z_texture_id(0)
    , m_model_object(nullptr)
    , m_object_max_z(0.f)
    , m_slicing_parameters(nullptr)
    , m_layer_height_profile_modified(false)
    , m_adaptive_quality(0.5f)
    , state(Unknown)
    , band_width(2.0f)
    , strength(0.005f)
    , last_object_id(-1)
    , last_z(0.0f)
    , last_action(LAYER_HEIGHT_EDIT_ACTION_INCREASE)
{
}

GLCanvas3D::LayersEditing::~LayersEditing()
{
    if (m_z_texture_id != 0)
    {
        glsafe(::glDeleteTextures(1, &m_z_texture_id));
        m_z_texture_id = 0;
    }
    delete m_slicing_parameters;
}

const float GLCanvas3D::LayersEditing::THICKNESS_BAR_WIDTH = 70.0f;

bool GLCanvas3D::LayersEditing::init(const std::string& vertex_shader_filename, const std::string& fragment_shader_filename)
{
    if (!m_shader.init(vertex_shader_filename, fragment_shader_filename))
        return false;

    glsafe(::glGenTextures(1, (GLuint*)&m_z_texture_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    return true;
}

void GLCanvas3D::LayersEditing::set_config(const DynamicPrintConfig* config)
{ 
    m_config = config;
    delete m_slicing_parameters;
    m_slicing_parameters = nullptr;
    m_layers_texture.valid = false;
}

void GLCanvas3D::LayersEditing::select_object(const Model &model, int object_id)
{
    const ModelObject *model_object_new = (object_id >= 0) ? model.objects[object_id] : nullptr;
    // Maximum height of an object changes when the object gets rotated or scaled.
    // Changing maximum height of an object will invalidate the layer heigth editing profile.
    // m_model_object->raw_bounding_box() is cached, therefore it is cheap even if this method is called frequently.
	float new_max_z = (model_object_new == nullptr) ? 0.f : model_object_new->raw_bounding_box().size().z();
	if (m_model_object != model_object_new || this->last_object_id != object_id || m_object_max_z != new_max_z ||
        (model_object_new != nullptr && m_model_object->id() != model_object_new->id())) {
        m_layer_height_profile.clear();
        m_layer_height_profile_modified = false;
        delete m_slicing_parameters;
        m_slicing_parameters   = nullptr;
        m_layers_texture.valid = false;
        this->last_object_id   = object_id;
        m_model_object         = model_object_new;
        m_object_max_z         = new_max_z;
    }
}

bool GLCanvas3D::LayersEditing::is_allowed() const
{
    return m_shader.is_initialized() && m_shader.get_shader()->shader_program_id > 0 && m_z_texture_id > 0;
}

bool GLCanvas3D::LayersEditing::is_enabled() const
{
    return m_enabled;
}

void GLCanvas3D::LayersEditing::set_enabled(bool enabled)
{
    m_enabled = is_allowed() && enabled;
}

void GLCanvas3D::LayersEditing::render_overlay(const GLCanvas3D& canvas) const
{
    if (!m_enabled)
        return;

    static const ImVec4 ORANGE(1.0f, 0.49f, 0.22f, 1.0f);

    const Size& cnv_size = canvas.get_canvas_size();
    float canvas_w = (float)cnv_size.get_width();
    float canvas_h = (float)cnv_size.get_height();

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    imgui.set_next_window_pos(canvas_w - imgui.get_style_scaling() * THICKNESS_BAR_WIDTH, canvas_h, ImGuiCond_Always, 1.0f, 1.0f);

    imgui.begin(_(L("Variable layer height")), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::PushStyleColor(ImGuiCol_Text, ORANGE);
    imgui.text(_(L("Left mouse button:")));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    imgui.text(_(L("Add detail")));

    ImGui::PushStyleColor(ImGuiCol_Text, ORANGE);
    imgui.text(_(L("Right mouse button:")));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    imgui.text(_(L("Remove detail")));

    ImGui::PushStyleColor(ImGuiCol_Text, ORANGE);
    imgui.text(_(L("Shift + Left mouse button:")));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    imgui.text(_(L("Reset to base")));

    ImGui::PushStyleColor(ImGuiCol_Text, ORANGE);
    imgui.text(_(L("Shift + Right mouse button:")));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    imgui.text(_(L("Smoothing")));

    ImGui::PushStyleColor(ImGuiCol_Text, ORANGE);
    imgui.text(_(L("Mouse wheel:")));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    imgui.text(_(L("Increase/decrease edit area")));
    
    ImGui::Separator();
    if (imgui.button(_(L("Adaptive"))))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), Event<float>(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, m_adaptive_quality));

    ImGui::SameLine();
    float text_align = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    imgui.text(_(L("Quality / Speed")));
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(_(L("Higher print quality versus higher print speed.")).ToUTF8());
        ImGui::EndTooltip();
    }

    ImGui::SameLine();
    float widget_align = ImGui::GetCursorPosX();
    ImGui::PushItemWidth(imgui.get_style_scaling() * 120.0f);
    m_adaptive_quality = clamp(0.0f, 1.f, m_adaptive_quality);
    ImGui::SliderFloat("", &m_adaptive_quality, 0.0f, 1.f, "%.2f");

    ImGui::Separator();
    if (imgui.button(_(L("Smooth"))))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), HeightProfileSmoothEvent(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, m_smooth_params));

    ImGui::SameLine();
    ImGui::SetCursorPosX(text_align);
    ImGui::AlignTextToFramePadding();
    imgui.text(_(L("Radius")));
    ImGui::SameLine();
    ImGui::SetCursorPosX(widget_align);
    ImGui::PushItemWidth(imgui.get_style_scaling() * 120.0f);
    int radius = (int)m_smooth_params.radius;
    if (ImGui::SliderInt("##1", &radius, 1, 10))
        m_smooth_params.radius = (unsigned int)radius;

    ImGui::SetCursorPosX(text_align);
    ImGui::AlignTextToFramePadding();
    imgui.text(_(L("Keep min")));
    ImGui::SameLine();
    if (ImGui::GetCursorPosX() < widget_align)  // because of line lenght after localization
        ImGui::SetCursorPosX(widget_align);

    ImGui::PushItemWidth(imgui.get_style_scaling() * 120.0f);
    imgui.checkbox("##2", m_smooth_params.keep_min);

    ImGui::Separator();
    if (imgui.button(_(L("Reset"))))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), SimpleEvent(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE));

    imgui.end();

    const Rect& bar_rect = get_bar_rect_viewport(canvas);
    render_active_object_annotations(canvas, bar_rect);
    render_profile(bar_rect);
}

float GLCanvas3D::LayersEditing::get_cursor_z_relative(const GLCanvas3D& canvas)
{
    const Vec2d mouse_pos = canvas.get_local_mouse_position();
    const Rect& rect = get_bar_rect_screen(canvas);
    float x = (float)mouse_pos(0);
    float y = (float)mouse_pos(1);
    float t = rect.get_top();
    float b = rect.get_bottom();

    return ((rect.get_left() <= x) && (x <= rect.get_right()) && (t <= y) && (y <= b)) ?
        // Inside the bar.
        (b - y - 1.0f) / (b - t - 1.0f) :
        // Outside the bar.
        -1000.0f;
}

bool GLCanvas3D::LayersEditing::bar_rect_contains(const GLCanvas3D& canvas, float x, float y)
{
    const Rect& rect = get_bar_rect_screen(canvas);
    return (rect.get_left() <= x) && (x <= rect.get_right()) && (rect.get_top() <= y) && (y <= rect.get_bottom());
}

Rect GLCanvas3D::LayersEditing::get_bar_rect_screen(const GLCanvas3D& canvas)
{
    const Size& cnv_size = canvas.get_canvas_size();
    float w = (float)cnv_size.get_width();
    float h = (float)cnv_size.get_height();

    return Rect(w - thickness_bar_width(canvas), 0.0f, w, h);
}

Rect GLCanvas3D::LayersEditing::get_bar_rect_viewport(const GLCanvas3D& canvas)
{
    const Size& cnv_size = canvas.get_canvas_size();
    float half_w = 0.5f * (float)cnv_size.get_width();
    float half_h = 0.5f * (float)cnv_size.get_height();

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    float inv_zoom = (float)wxGetApp().plater()->get_camera().get_inv_zoom();
#else
    float inv_zoom = (float)canvas.get_camera().get_inv_zoom();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

    return Rect((half_w - thickness_bar_width(canvas)) * inv_zoom, half_h * inv_zoom, half_w * inv_zoom, -half_h * inv_zoom);
}

bool GLCanvas3D::LayersEditing::is_initialized() const
{
    return m_shader.is_initialized();
}

std::string GLCanvas3D::LayersEditing::get_tooltip(const GLCanvas3D& canvas) const
{
    std::string ret;
    if (m_enabled && (m_layer_height_profile.size() >= 4))
    {
        float z = get_cursor_z_relative(canvas);
        if (z != -1000.0f)
        {
            z *= m_object_max_z;

            float h = 0.0f;
            for (size_t i = m_layer_height_profile.size() - 2; i >= 2; i -= 2)
            {
                float zi = m_layer_height_profile[i];
                float zi_1 = m_layer_height_profile[i - 2];
                if ((zi_1 <= z) && (z <= zi))
                {
                    float dz = zi - zi_1;
                    h = (dz != 0.0f) ? lerp(m_layer_height_profile[i - 1], m_layer_height_profile[i + 1], (z - zi_1) / dz) : m_layer_height_profile[i + 1];
                    break;
                }
            }
            if (h > 0.0f)
                ret = std::to_string(h);
        }
    }
    return ret;
}

void GLCanvas3D::LayersEditing::render_active_object_annotations(const GLCanvas3D& canvas, const Rect& bar_rect) const
{
    m_shader.start_using();

    m_shader.set_uniform("z_to_texture_row", float(m_layers_texture.cells - 1) / (float(m_layers_texture.width) * m_object_max_z));
	m_shader.set_uniform("z_texture_row_to_normalized", 1.0f / (float)m_layers_texture.height);
    m_shader.set_uniform("z_cursor", m_object_max_z * this->get_cursor_z_relative(canvas));
    m_shader.set_uniform("z_cursor_band_width", band_width);
    m_shader.set_uniform("object_max_z", m_object_max_z);

    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));

    // Render the color bar
    float l = bar_rect.get_left();
    float r = bar_rect.get_right();
    float t = bar_rect.get_top();
    float b = bar_rect.get_bottom();

    ::glBegin(GL_QUADS);
    ::glNormal3f(0.0f, 0.0f, 1.0f);
    ::glTexCoord2f(0.0f, 0.0f); ::glVertex2f(l, b);
    ::glTexCoord2f(1.0f, 0.0f); ::glVertex2f(r, b);
    ::glTexCoord2f(1.0f, 1.0f); ::glVertex2f(r, t);
    ::glTexCoord2f(0.0f, 1.0f); ::glVertex2f(l, t);
    glsafe(::glEnd());
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    m_shader.stop_using();
}

void GLCanvas3D::LayersEditing::render_profile(const Rect& bar_rect) const
{
    //FIXME show some kind of legend.

    if (!m_slicing_parameters)
        return;

    // Make the vertical bar a bit wider so the layer height curve does not touch the edge of the bar region.
    float scale_x = bar_rect.get_width() / (float)(1.12 * m_slicing_parameters->max_layer_height);
    float scale_y = bar_rect.get_height() / m_object_max_z;
    float x = bar_rect.get_left() + (float)m_slicing_parameters->layer_height * scale_x;

    // Baseline
    glsafe(::glColor3f(0.0f, 0.0f, 0.0f));
    ::glBegin(GL_LINE_STRIP);
    ::glVertex2f(x, bar_rect.get_bottom());
    ::glVertex2f(x, bar_rect.get_top());
    glsafe(::glEnd());

    // Curve
    glsafe(::glColor3f(0.0f, 0.0f, 1.0f));
    ::glBegin(GL_LINE_STRIP);
    for (unsigned int i = 0; i < m_layer_height_profile.size(); i += 2)
        ::glVertex2f(bar_rect.get_left() + (float)m_layer_height_profile[i + 1] * scale_x, bar_rect.get_bottom() + (float)m_layer_height_profile[i] * scale_y);
    glsafe(::glEnd());
}

void GLCanvas3D::LayersEditing::render_volumes(const GLCanvas3D& canvas, const GLVolumeCollection &volumes) const
{
    assert(this->is_allowed());
    assert(this->last_object_id != -1);
    GLint shader_id = m_shader.get_shader()->shader_program_id;
    assert(shader_id > 0);

    GLint current_program_id;
    glsafe(::glGetIntegerv(GL_CURRENT_PROGRAM, &current_program_id));
    if (shader_id > 0 && shader_id != current_program_id)
        // The layer editing shader is not yet active. Activate it.
        glsafe(::glUseProgram(shader_id));
    else
        // The layer editing shader was already active.
        current_program_id = -1;

    GLint z_to_texture_row_id               = ::glGetUniformLocation(shader_id, "z_to_texture_row");
    GLint z_texture_row_to_normalized_id    = ::glGetUniformLocation(shader_id, "z_texture_row_to_normalized");
    GLint z_cursor_id                       = ::glGetUniformLocation(shader_id, "z_cursor");
    GLint z_cursor_band_width_id            = ::glGetUniformLocation(shader_id, "z_cursor_band_width");
    GLint world_matrix_id                   = ::glGetUniformLocation(shader_id, "volume_world_matrix");
    GLint object_max_z_id                   = ::glGetUniformLocation(shader_id, "object_max_z");
    glcheck();

    if (z_to_texture_row_id != -1 && z_texture_row_to_normalized_id != -1 && z_cursor_id != -1 && z_cursor_band_width_id != -1 && world_matrix_id != -1) 
    {
        const_cast<LayersEditing*>(this)->generate_layer_height_texture();

        // Uniforms were resolved, go ahead using the layer editing shader.
        glsafe(::glUniform1f(z_to_texture_row_id, GLfloat(m_layers_texture.cells - 1) / (GLfloat(m_layers_texture.width) * GLfloat(m_object_max_z))));
        glsafe(::glUniform1f(z_texture_row_to_normalized_id, GLfloat(1.0f / m_layers_texture.height)));
        glsafe(::glUniform1f(z_cursor_id, GLfloat(m_object_max_z) * GLfloat(this->get_cursor_z_relative(canvas))));
        glsafe(::glUniform1f(z_cursor_band_width_id, GLfloat(this->band_width)));
        // Initialize the layer height texture mapping.
        GLsizei w = (GLsizei)m_layers_texture.width;
        GLsizei h = (GLsizei)m_layers_texture.height;
        GLsizei half_w = w / 2;
        GLsizei half_h = h / 2;
        glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, half_w, half_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
        glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data()));
        glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, half_w, half_h, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data() + m_layers_texture.width * m_layers_texture.height * 4));
        for (const GLVolume* glvolume : volumes.volumes) {
            // Render the object using the layer editing shader and texture.
            if (! glvolume->is_active || glvolume->composite_id.object_id != this->last_object_id || glvolume->is_modifier)
                continue;
            if (world_matrix_id != -1)
                glsafe(::glUniformMatrix4fv(world_matrix_id, 1, GL_FALSE, (const GLfloat*)glvolume->world_matrix().cast<float>().data()));
            if (object_max_z_id != -1)
                glsafe(::glUniform1f(object_max_z_id, GLfloat(0)));
            glvolume->render();
        }
        // Revert back to the previous shader.
        glBindTexture(GL_TEXTURE_2D, 0);
        if (current_program_id > 0)
            glsafe(::glUseProgram(current_program_id));
    } 
    else 
    {
        // Something went wrong. Just render the object.
        assert(false);
        for (const GLVolume* glvolume : volumes.volumes) {
            // Render the object using the layer editing shader and texture.
			if (!glvolume->is_active || glvolume->composite_id.object_id != this->last_object_id || glvolume->is_modifier)
				continue;
            glsafe(::glUniformMatrix4fv(world_matrix_id, 1, GL_FALSE, (const GLfloat*)glvolume->world_matrix().cast<float>().data()));
			glvolume->render();
		}
	}
}

void GLCanvas3D::LayersEditing::adjust_layer_height_profile()
{
	this->update_slicing_parameters();
	PrintObject::update_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile);
	Slic3r::adjust_layer_height_profile(*m_slicing_parameters, m_layer_height_profile, this->last_z, this->strength, this->band_width, this->last_action);
	m_layer_height_profile_modified = true;
    m_layers_texture.valid = false;
}

void GLCanvas3D::LayersEditing::reset_layer_height_profile(GLCanvas3D& canvas)
{
	const_cast<ModelObject*>(m_model_object)->layer_height_profile.clear();
    m_layer_height_profile.clear();
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLCanvas3D::LayersEditing::adaptive_layer_height_profile(GLCanvas3D& canvas, float quality_factor)
{
    this->update_slicing_parameters();
    m_layer_height_profile = layer_height_profile_adaptive(*m_slicing_parameters, *m_model_object, quality_factor);
    const_cast<ModelObject*>(m_model_object)->layer_height_profile = m_layer_height_profile;
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLCanvas3D::LayersEditing::smooth_layer_height_profile(GLCanvas3D& canvas, const HeightProfileSmoothingParams& smoothing_params)
{
    this->update_slicing_parameters();
    m_layer_height_profile = smooth_height_profile(m_layer_height_profile, *m_slicing_parameters, smoothing_params);
    const_cast<ModelObject*>(m_model_object)->layer_height_profile = m_layer_height_profile;
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLCanvas3D::LayersEditing::generate_layer_height_texture()
{
	this->update_slicing_parameters();
	// Always try to update the layer height profile.
    bool update = ! m_layers_texture.valid;
    if (PrintObject::update_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile)) {
        // Initialized to the default value.
        m_layer_height_profile_modified = false;
        update = true;
    }
    // Update if the layer height profile was changed, or when the texture is not valid.
    if (! update && ! m_layers_texture.data.empty() && m_layers_texture.cells > 0)
        // Texture is valid, don't update.
        return; 

    if (m_layers_texture.data.empty()) {
        m_layers_texture.width  = 1024;
        m_layers_texture.height = 1024;
        m_layers_texture.levels = 2;
        m_layers_texture.data.assign(m_layers_texture.width * m_layers_texture.height * 5, 0);
    }

    bool level_of_detail_2nd_level = true;
    m_layers_texture.cells = Slic3r::generate_layer_height_texture(
        *m_slicing_parameters, 
        Slic3r::generate_object_layers(*m_slicing_parameters, m_layer_height_profile), 
		m_layers_texture.data.data(), m_layers_texture.height, m_layers_texture.width, level_of_detail_2nd_level);
	m_layers_texture.valid = true;
}

void GLCanvas3D::LayersEditing::accept_changes(GLCanvas3D& canvas)
{
    if (last_object_id >= 0) {
        if (m_layer_height_profile_modified) {
            wxGetApp().plater()->take_snapshot(_(L("Variable layer height - Manual edit")));
            const_cast<ModelObject*>(m_model_object)->layer_height_profile = m_layer_height_profile;
			canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        }
    }
    m_layer_height_profile_modified = false;
}

void GLCanvas3D::LayersEditing::update_slicing_parameters()
{
	if (m_slicing_parameters == nullptr) {
		m_slicing_parameters = new SlicingParameters();
    	*m_slicing_parameters = PrintObject::slicing_parameters(*m_config, *m_model_object, m_object_max_z);
    }
}

float GLCanvas3D::LayersEditing::thickness_bar_width(const GLCanvas3D &canvas)
{
    return
#if ENABLE_RETINA_GL
        canvas.get_canvas_size().get_scale_factor()
#else
        canvas.get_wxglcanvas()->GetContentScaleFactor()
#endif
         * THICKNESS_BAR_WIDTH;
}


const Point GLCanvas3D::Mouse::Drag::Invalid_2D_Point(INT_MAX, INT_MAX);
const Vec3d GLCanvas3D::Mouse::Drag::Invalid_3D_Point(DBL_MAX, DBL_MAX, DBL_MAX);
const int GLCanvas3D::Mouse::Drag::MoveThresholdPx = 5;

GLCanvas3D::Mouse::Drag::Drag()
    : start_position_2D(Invalid_2D_Point)
    , start_position_3D(Invalid_3D_Point)
    , move_volume_idx(-1)
    , move_requires_threshold(false)
    , move_start_threshold_position_2D(Invalid_2D_Point)
{
}

GLCanvas3D::Mouse::Mouse()
    : dragging(false)
    , position(DBL_MAX, DBL_MAX)
    , scene_position(DBL_MAX, DBL_MAX, DBL_MAX)
    , ignore_left_up(false)
{
}

const unsigned char GLCanvas3D::WarningTexture::Background_Color[3] = { 120, 120, 120 };//{ 9, 91, 134 };
const unsigned char GLCanvas3D::WarningTexture::Opacity = 255;

GLCanvas3D::WarningTexture::WarningTexture()
    : GUI::GLTexture()
    , m_original_width(0)
    , m_original_height(0)
{
}

void GLCanvas3D::WarningTexture::activate(WarningTexture::Warning warning, bool state, const GLCanvas3D& canvas)
{
    auto it = std::find(m_warnings.begin(), m_warnings.end(), warning);

    if (state) {
        if (it != m_warnings.end()) // this warning is already set to be shown
            return;

        m_warnings.emplace_back(warning);
        std::sort(m_warnings.begin(), m_warnings.end());
    }
    else {
        if (it == m_warnings.end()) // deactivating something that is not active is an easy task
            return;

        m_warnings.erase(it);
        if (m_warnings.empty()) { // nothing remains to be shown
            reset();
            m_msg_text = "";// save information for rescaling
            return;
        }
    }

    // Look at the end of our vector and generate proper texture.
    std::string text;
    bool red_colored = false;
    switch (m_warnings.back()) {
        case ObjectOutside      : text = L("An object outside the print area was detected"); break;
        case ToolpathOutside    : text = L("A toolpath outside the print area was detected"); break;
        case SlaSupportsOutside : text = L("SLA supports outside the print area were detected"); break;
        case SomethingNotShown  : text = L("Some objects are not visible"); break;
        case ObjectClashed: {
            text = L("An object outside the print area was detected\n"
                     "Resolve the current problem to continue slicing");
            red_colored = true;
            break;
        }
    }

    generate(text, canvas, true, red_colored); // GUI::GLTexture::reset() is called at the beginning of generate(...)

    // save information for rescaling
    m_msg_text = text;
    m_is_colored_red = red_colored;
}


#ifdef __WXMSW__
static bool is_font_cleartype(const wxFont &font)
{
    // Native font description: on MSW, it is a version number plus the content of LOGFONT, separated by semicolon.
    wxString font_desc = font.GetNativeFontInfoDesc();
    // Find the quality field.
    wxString sep(";");
    size_t startpos = 0;
    for (size_t i = 0; i < 12; ++ i)
        startpos = font_desc.find(sep, startpos + 1);
    ++ startpos;
    size_t endpos = font_desc.find(sep, startpos);
    int quality = wxAtoi(font_desc(startpos, endpos - startpos));
    return quality == CLEARTYPE_QUALITY;
}

// ClearType produces renders, which are difficult to convert into an alpha blended OpenGL texture.
// Therefore it is better to disable it, though Vojtech found out, that the font returned with ClearType
// disabled is signifcantly thicker than the default ClearType font.
// This function modifies the font provided.
static void msw_disable_cleartype(wxFont &font)
{
    // Native font description: on MSW, it is a version number plus the content of LOGFONT, separated by semicolon.
    wxString font_desc = font.GetNativeFontInfoDesc();
    // Find the quality field.
    wxString sep(";");
    size_t startpos_weight = 0;
    for (size_t i = 0; i < 5; ++ i)
        startpos_weight = font_desc.find(sep, startpos_weight + 1);
    ++ startpos_weight;
    size_t endpos_weight = font_desc.find(sep, startpos_weight);
    // Parse the weight field.
    unsigned int weight = wxAtoi(font_desc(startpos_weight, endpos_weight - startpos_weight));
    size_t startpos = endpos_weight;
    for (size_t i = 0; i < 6; ++ i)
        startpos = font_desc.find(sep, startpos + 1);
    ++ startpos;
    size_t endpos = font_desc.find(sep, startpos);
    int quality = wxAtoi(font_desc(startpos, endpos - startpos));
    if (quality == CLEARTYPE_QUALITY) {
        // Replace the weight with a smaller value to compensate the weight of non ClearType font.
        wxString sweight    = std::to_string(weight * 2 / 4);
        size_t   len_weight = endpos_weight - startpos_weight;
        wxString squality   = std::to_string(ANTIALIASED_QUALITY);
        font_desc.replace(startpos_weight, len_weight, sweight);
        font_desc.replace(startpos + sweight.size() - len_weight, endpos - startpos, squality);
        font.SetNativeFontInfo(font_desc);
        wxString font_desc2 = font.GetNativeFontInfoDesc();
    }
    wxString font_desc2 = font.GetNativeFontInfoDesc();
}
#endif /* __WXMSW__ */

bool GLCanvas3D::WarningTexture::generate(const std::string& msg_utf8, const GLCanvas3D& canvas, bool compress, bool red_colored/* = false*/)
{
    reset();

    if (msg_utf8.empty())
        return false;

    wxString msg = _(msg_utf8);

    wxMemoryDC memDC;

#ifdef __WXMSW__
    // set scaled application normal font as default font 
    wxFont font = wxGetApp().normal_font();
#else
    // select default font
    const float scale = canvas.get_canvas_size().get_scale_factor();
#if ENABLE_RETINA_GL
    // For non-visible or non-created window getBackingScaleFactor function return 0.0 value.
    // And using of the zero scale causes a crash, when we trying to draw text to the (0,0) rectangle
    // https://github.com/prusa3d/PrusaSlicer/issues/3916
    if (scale <= 0.0f)
        return false;
#endif
    wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).Scale(scale);
#endif

    font.MakeLarger();
    font.MakeBold();
    memDC.SetFont(font);

    // calculates texture size
    wxCoord w, h;
    memDC.GetMultiLineTextExtent(msg, &w, &h);

    m_original_width = (int)w;
    m_original_height = (int)h;
    m_width = (int)next_highest_power_of_2((uint32_t)w);
	m_height = (int)next_highest_power_of_2((uint32_t)h);

    // generates bitmap
    wxBitmap bitmap(m_width, m_height);

    memDC.SelectObject(bitmap);
    memDC.SetBackground(wxBrush(*wxBLACK));
    memDC.Clear();

    // draw message
    memDC.SetTextForeground(*wxRED);
	memDC.DrawLabel(msg, wxRect(0,0, m_original_width, m_original_height), wxALIGN_CENTER);

    memDC.SelectObject(wxNullBitmap);

    // Convert the bitmap into a linear data ready to be loaded into the GPU.
    wxImage image = bitmap.ConvertToImage();

    // prepare buffer
    std::vector<unsigned char> data(4 * m_width * m_height, 0);
    const unsigned char *src = image.GetData();
    for (int h = 0; h < m_height; ++h)
    {
        unsigned char* dst = data.data() + 4 * h * m_width;
        for (int w = 0; w < m_width; ++w)
        {
            *dst++ = 255;
            if (red_colored) {
                *dst++ = 72; // 204
                *dst++ = 65; // 204
            } else {
                *dst++ = 255;
                *dst++ = 255;
            }
			*dst++ = (unsigned char)std::min<int>(255, *src);
            src += 3;
        }
    }

    // sends buffer to gpu
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &m_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint)m_id));
    if (compress && GLEW_EXT_texture_compression_s3tc)
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    return true;
}

void GLCanvas3D::WarningTexture::render(const GLCanvas3D& canvas) const
{
    if (m_warnings.empty())
        return;

    if ((m_id > 0) && (m_original_width > 0) && (m_original_height > 0) && (m_width > 0) && (m_height > 0))
    {
        const Size& cnv_size = canvas.get_canvas_size();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
        float inv_zoom = (float)wxGetApp().plater()->get_camera().get_inv_zoom();
#else
        float inv_zoom = (float)canvas.get_camera().get_inv_zoom();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
        float left = (-0.5f * (float)m_original_width) * inv_zoom;
        float top = (-0.5f * (float)cnv_size.get_height() + (float)m_original_height + 2.0f) * inv_zoom;
        float right = left + (float)m_original_width * inv_zoom;
        float bottom = top - (float)m_original_height * inv_zoom;

        float uv_left = 0.0f;
        float uv_top = 0.0f;
        float uv_right = (float)m_original_width / (float)m_width;
        float uv_bottom = (float)m_original_height / (float)m_height;

        GLTexture::Quad_UVs uvs;
        uvs.left_top = { uv_left, uv_top };
        uvs.left_bottom = { uv_left, uv_bottom };
        uvs.right_bottom = { uv_right, uv_bottom };
        uvs.right_top = { uv_right, uv_top };

        GLTexture::render_sub_texture(m_id, left, right, bottom, top, uvs);
    }
}

void GLCanvas3D::WarningTexture::msw_rescale(const GLCanvas3D& canvas)
{
    if (m_msg_text.empty())
        return;

    generate(m_msg_text, canvas, true, m_is_colored_red);
}

const unsigned char GLCanvas3D::LegendTexture::Squares_Border_Color[3] = { 64, 64, 64 };
const unsigned char GLCanvas3D::LegendTexture::Default_Background_Color[3] = { (unsigned char)(DEFAULT_BG_LIGHT_COLOR[0] * 255.0f), (unsigned char)(DEFAULT_BG_LIGHT_COLOR[1] * 255.0f), (unsigned char)(DEFAULT_BG_LIGHT_COLOR[2] * 255.0f) };
const unsigned char GLCanvas3D::LegendTexture::Error_Background_Color[3] = { (unsigned char)(ERROR_BG_LIGHT_COLOR[0] * 255.0f), (unsigned char)(ERROR_BG_LIGHT_COLOR[1] * 255.0f), (unsigned char)(ERROR_BG_LIGHT_COLOR[2] * 255.0f) };
const unsigned char GLCanvas3D::LegendTexture::Opacity = 255;

GLCanvas3D::LegendTexture::LegendTexture()
    : GUI::GLTexture()
    , m_original_width(0)
    , m_original_height(0)
{
}

void GLCanvas3D::LegendTexture::fill_color_print_legend_items(  const GLCanvas3D& canvas,
                                                                const std::vector<float>& colors_in,
                                                                std::vector<float>& colors,
                                                                std::vector<std::string>& cp_legend_items)
{
    std::vector<CustomGCode::Item> custom_gcode_per_print_z = wxGetApp().plater()->model().custom_gcode_per_print_z.gcodes;

    const int extruders_cnt = wxGetApp().extruders_edited_cnt();
    if (extruders_cnt == 1) 
    {
        if (custom_gcode_per_print_z.empty()) {
            cp_legend_items.emplace_back(I18N::translate_utf8(L("Default print color")));
            colors = colors_in;
            return;
        }
        std::vector<std::pair<double, double>> cp_values;
        cp_values.reserve(custom_gcode_per_print_z.size());
        
        std::vector<double> print_zs = canvas.get_current_print_zs(true);
        for (auto custom_code : custom_gcode_per_print_z)
        {
            if (custom_code.gcode != ColorChangeCode)
                continue;
            auto lower_b = std::lower_bound(print_zs.begin(), print_zs.end(), custom_code.print_z - Slic3r::DoubleSlider::epsilon());

            if (lower_b == print_zs.end())
                continue;

            double current_z = *lower_b;
            double previous_z = lower_b == print_zs.begin() ? 0.0 : *(--lower_b);

            // to avoid duplicate values, check adding values
            if (cp_values.empty() ||
                !(cp_values.back().first == previous_z && cp_values.back().second == current_z))
                cp_values.emplace_back(std::pair<double, double>(previous_z, current_z));
        }

        const auto items_cnt = (int)cp_values.size();
        if (items_cnt == 0) // There is no one color change, but there is/are some pause print or custom Gcode
        {
            cp_legend_items.emplace_back(I18N::translate_utf8(L("Default print color")));
            cp_legend_items.emplace_back(I18N::translate_utf8(L("Pause print or custom G-code")));
            colors = colors_in;
            return;
        }

        const int color_cnt = (int)colors_in.size() / 4;
        colors.resize(colors_in.size(), 0.0);
                
        ::memcpy((void*)(colors.data()), (const void*)(colors_in.data() + (color_cnt - 1) * 4), 4 * sizeof(float));
        cp_legend_items.emplace_back(I18N::translate_utf8(L("Pause print or custom G-code")));
        size_t color_pos = 4;

        for (int i = items_cnt; i >= 0; --i, color_pos+=4)
        {
            // update colors for color print item
            ::memcpy((void*)(colors.data() + color_pos), (const void*)(colors_in.data() + i * 4), 4 * sizeof(float));

            // create label for color print item
            std::string id_str = std::to_string(i + 1) + ": ";

            if (i == 0) {
                cp_legend_items.emplace_back(id_str + (boost::format(I18N::translate_utf8(L("up to %.2f mm"))) % cp_values[0].first).str());
                break;
            }
            if (i == items_cnt) {
                cp_legend_items.emplace_back(id_str + (boost::format(I18N::translate_utf8(L("above %.2f mm"))) % cp_values[i - 1].second).str());
                continue;
            }

            cp_legend_items.emplace_back(id_str + (boost::format(I18N::translate_utf8(L("%.2f - %.2f mm"))) % cp_values[i - 1].second % cp_values[i].first).str());
        }
    }
    else
    {
        // colors = colors_in;
        const int color_cnt = (int)colors_in.size() / 4;
        colors.resize(colors_in.size(), 0.0);

        ::memcpy((void*)(colors.data()), (const void*)(colors_in.data()), 4 * extruders_cnt * sizeof(float));
        size_t color_pos = 4 * extruders_cnt;
        size_t color_in_pos = 4 * (color_cnt - 1);
        
        for (unsigned int i = 0; i < (unsigned int)extruders_cnt; ++i)
            cp_legend_items.emplace_back((boost::format(I18N::translate_utf8(L("Extruder %d"))) % (i + 1)).str());

        ::memcpy((void*)(colors.data() + color_pos), (const void*)(colors_in.data() + color_in_pos), 4 * sizeof(float));
        color_pos += 4;
        color_in_pos -= 4;
        cp_legend_items.emplace_back(I18N::translate_utf8(L("Pause print or custom G-code")));

        int cnt = custom_gcode_per_print_z.size();
        int color_change_idx = color_cnt - extruders_cnt;
        for (int i = cnt-1; i >= 0; --i)
            if (custom_gcode_per_print_z[i].gcode == ColorChangeCode) {
                ::memcpy((void*)(colors.data() + color_pos), (const void*)(colors_in.data() + color_in_pos), 4 * sizeof(float));
                color_pos += 4;
                color_in_pos -= 4;

                // create label for color change item
                std::string id_str = std::to_string(color_change_idx--) + ": ";

                cp_legend_items.emplace_back(id_str + (boost::format(I18N::translate_utf8(L("Color change for Extruder %d at %.2f mm"))) % custom_gcode_per_print_z[i].extruder % custom_gcode_per_print_z[i].print_z).str());
            }
    }
}

bool GLCanvas3D::LegendTexture::generate(const GCodePreviewData& preview_data, const std::vector<float>& tool_colors_in, const GLCanvas3D& canvas, bool compress)
{
    reset();

    // collects items to render
    auto title = _(preview_data.get_legend_title());

    std::vector<std::string> cp_legend_items;
    std::vector<float> cp_colors;

    if (preview_data.extrusion.view_type == GCodePreviewData::Extrusion::ColorPrint)
    {
        cp_legend_items.reserve(cp_colors.size());
        fill_color_print_legend_items(canvas, tool_colors_in, cp_colors, cp_legend_items);
    }

    const std::vector<float>& tool_colors = preview_data.extrusion.view_type == GCodePreviewData::Extrusion::ColorPrint ? cp_colors : tool_colors_in;
    const GCodePreviewData::LegendItemsList& items = preview_data.get_legend_items(tool_colors, cp_legend_items);

    unsigned int items_count = (unsigned int)items.size();
    if (items_count == 0)
        // nothing to render, return
        return false;

    wxMemoryDC memDC;
    wxMemoryDC mask_memDC;

    // calculate scaling
    const float scale_gl = canvas.get_canvas_size().get_scale_factor();
#if ENABLE_RETINA_GL
    // For non-visible or non-created window getBackingScaleFactor function return 0.0 value.
    // And using of the zero scale causes a crash, when we trying to draw text to the (0,0) rectangle
    if (scale_gl <= 0.0f)
        return false;
#endif
    const float scale = scale_gl * wxGetApp().em_unit()*0.1; // get scale from em_unit() value, because of get_scale_factor() return 1 
    const int scaled_square = std::floor((float)Px_Square * scale);
    const int scaled_title_offset = Px_Title_Offset * scale;
    const int scaled_text_offset = Px_Text_Offset * scale;
    const int scaled_square_contour = Px_Square_Contour * scale;
    const int scaled_border = Px_Border * scale;

#ifdef __WXMSW__
    // set scaled application normal font as default font 
    wxFont font = wxGetApp().normal_font();

    // Disabling ClearType works, but the font returned is very different (much thicker) from the default.
//    msw_disable_cleartype(font);
//    bool cleartype = is_font_cleartype(font);
#else
    // select default font
    wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).Scale(scale_gl);
//    bool cleartype = false;
#endif /* __WXMSW__ */

    memDC.SetFont(font);
    mask_memDC.SetFont(font);

    // calculates texture size
    wxCoord w, h;
    memDC.GetTextExtent(title, &w, &h);
    int title_width = (int)w;
    int title_height = (int)h;

    int max_text_width = 0;
    int max_text_height = 0;
    for (const GCodePreviewData::LegendItem& item : items)
    {
        memDC.GetTextExtent(GUI::from_u8(item.text), &w, &h);
        max_text_width = std::max(max_text_width, (int)w);
        max_text_height = std::max(max_text_height, (int)h);
    }

    m_original_width = std::max(2 * scaled_border + title_width, 2 * (scaled_border + scaled_square_contour) + scaled_square + scaled_text_offset + max_text_width);
    m_original_height = 2 * (scaled_border + scaled_square_contour) + title_height + scaled_title_offset + items_count * scaled_square;
    if (items_count > 1)
        m_original_height += (items_count - 1) * scaled_square_contour;

    m_width = (int)next_highest_power_of_2((uint32_t)m_original_width);
    m_height = (int)next_highest_power_of_2((uint32_t)m_original_height);

    // generates bitmap
    wxBitmap bitmap(m_width, m_height);
    wxBitmap mask(m_width, m_height);

    memDC.SelectObject(bitmap);
    mask_memDC.SelectObject(mask);

    memDC.SetBackground(wxBrush(*wxBLACK));
    mask_memDC.SetBackground(wxBrush(*wxBLACK));

    memDC.Clear();
    mask_memDC.Clear();

    // draw title
    memDC.SetTextForeground(*wxWHITE);
	mask_memDC.SetTextForeground(*wxRED);

    int title_x = scaled_border;
    int title_y = scaled_border;
    memDC.DrawText(title, title_x, title_y);
    mask_memDC.DrawText(title, title_x, title_y);

    // draw icons contours as background
    int squares_contour_x = scaled_border;
    int squares_contour_y = scaled_border + title_height + scaled_title_offset;
    int squares_contour_width = scaled_square + 2 * scaled_square_contour;
    int squares_contour_height = items_count * scaled_square + 2 * scaled_square_contour;
    if (items_count > 1)
        squares_contour_height += (items_count - 1) * scaled_square_contour;

    wxColour color(Squares_Border_Color[0], Squares_Border_Color[1], Squares_Border_Color[2]);
    wxPen pen(color);
    wxBrush brush(color);
    memDC.SetPen(pen);
    memDC.SetBrush(brush);
    memDC.DrawRectangle(wxRect(squares_contour_x, squares_contour_y, squares_contour_width, squares_contour_height));

    // draw items (colored icon + text)
    int icon_x = squares_contour_x + scaled_square_contour;
    int icon_x_inner = icon_x + 1;
    int icon_y = squares_contour_y + scaled_square_contour;
    int icon_y_step = scaled_square + scaled_square_contour;

    int text_x = icon_x + scaled_square + scaled_text_offset;
    int text_y_offset = (scaled_square - max_text_height) / 2;

    int px_inner_square = scaled_square - 2;

    for (const GCodePreviewData::LegendItem& item : items)
    {
        // draw darker icon perimeter
        const std::vector<unsigned char>& item_color_bytes = item.color.as_bytes();
        wxImage::HSVValue dark_hsv = wxImage::RGBtoHSV(wxImage::RGBValue(item_color_bytes[0], item_color_bytes[1], item_color_bytes[2]));
        dark_hsv.value *= 0.75;
        wxImage::RGBValue dark_rgb = wxImage::HSVtoRGB(dark_hsv);
        color.Set(dark_rgb.red, dark_rgb.green, dark_rgb.blue, item_color_bytes[3]);
        pen.SetColour(color);
        brush.SetColour(color);
        memDC.SetPen(pen);
        memDC.SetBrush(brush);
        memDC.DrawRectangle(wxRect(icon_x, icon_y, scaled_square, scaled_square));

        // draw icon interior
        color.Set(item_color_bytes[0], item_color_bytes[1], item_color_bytes[2], item_color_bytes[3]);
        pen.SetColour(color);
        brush.SetColour(color);
        memDC.SetPen(pen);
        memDC.SetBrush(brush);
        memDC.DrawRectangle(wxRect(icon_x_inner, icon_y + 1, px_inner_square, px_inner_square));

        // draw text
        mask_memDC.DrawText(GUI::from_u8(item.text), text_x, icon_y + text_y_offset);

        // update y
        icon_y += icon_y_step;
    }

    memDC.SelectObject(wxNullBitmap);
    mask_memDC.SelectObject(wxNullBitmap);

    // Convert the bitmap into a linear data ready to be loaded into the GPU.
    wxImage image = bitmap.ConvertToImage();
    wxImage mask_image = mask.ConvertToImage();

    // prepare buffer
    std::vector<unsigned char> data(4 * m_width * m_height, 0);
	const unsigned char *src_image = image.GetData();
    const unsigned char *src_mask  = mask_image.GetData();
	for (int h = 0; h < m_height; ++h)
    {
        int hh = h * m_width;
        unsigned char* px_ptr = data.data() + 4 * hh;
        for (int w = 0; w < m_width; ++w)
        {
			if (w >= squares_contour_x && w < squares_contour_x + squares_contour_width &&
				h >= squares_contour_y && h < squares_contour_y + squares_contour_height) {
				// Color palette, use the color verbatim.
				*px_ptr++ = *src_image++;
				*px_ptr++ = *src_image++;
				*px_ptr++ = *src_image++;
				*px_ptr++ = 255;
			} else {
				// Text or background
				unsigned char alpha = *src_mask;
				// Compensate the white color for the 50% opacity reduction at the character edges.
                //unsigned char color = (unsigned char)floor(alpha * 255.f / (128.f + 0.5f * alpha));
                unsigned char color = alpha;
				*px_ptr++ = color;
				*px_ptr++ = color; // *src_mask ++;
				*px_ptr++ = color; // *src_mask ++;
				*px_ptr++ = 128 + (alpha / 2); // (alpha > 0) ? 255 : 128;
				src_image += 3;
			}
            src_mask += 3;
        }
    }

    // sends buffer to gpu
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &m_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint)m_id));
    if (compress && GLEW_EXT_texture_compression_s3tc)
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)m_width, (GLsizei)m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void*)data.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    return true;
}

void GLCanvas3D::LegendTexture::render(const GLCanvas3D& canvas) const
{
    if ((m_id > 0) && (m_original_width > 0) && (m_original_height > 0) && (m_width > 0) && (m_height > 0))
    {
        const Size& cnv_size = canvas.get_canvas_size();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
        float inv_zoom = (float)wxGetApp().plater()->get_camera().get_inv_zoom();
#else
        float inv_zoom = (float)canvas.get_camera().get_inv_zoom();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
        float left = (-0.5f * (float)cnv_size.get_width()) * inv_zoom;
        float top = (0.5f * (float)cnv_size.get_height()) * inv_zoom;
        float right = left + (float)m_original_width * inv_zoom;
        float bottom = top - (float)m_original_height * inv_zoom;

        float uv_left = 0.0f;
        float uv_top = 0.0f;
        float uv_right = (float)m_original_width / (float)m_width;
        float uv_bottom = (float)m_original_height / (float)m_height;

        GLTexture::Quad_UVs uvs;
        uvs.left_top = { uv_left, uv_top };
        uvs.left_bottom = { uv_left, uv_bottom };
        uvs.right_bottom = { uv_right, uv_bottom };
        uvs.right_top = { uv_right, uv_top };

        GLTexture::render_sub_texture(m_id, left, right, bottom, top, uvs);
    }
}

void GLCanvas3D::Labels::render(const std::vector<const ModelInstance*>& sorted_instances) const
{
    if (!m_enabled || !is_shown())
        return;

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    const Camera& camera = wxGetApp().plater()->get_camera();
#else
    const Camera& camera = m_canvas.get_camera();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    const Model* model = m_canvas.get_model();
    if (model == nullptr)
        return;

    Transform3d world_to_eye = camera.get_view_matrix();
    Transform3d world_to_screen = camera.get_projection_matrix() * world_to_eye;
    const std::array<int, 4>& viewport = camera.get_viewport();

    struct Owner
    {
        int obj_idx;
        int inst_idx;
        size_t model_instance_id;
        BoundingBoxf3 world_box;
        double eye_center_z;
        std::string title;
        std::string label;
        std::string print_order;
        bool selected;
    };

    // collect owners world bounding boxes and data from volumes
    std::vector<Owner> owners;
    const GLVolumeCollection& volumes = m_canvas.get_volumes();
    for (const GLVolume* volume : volumes.volumes) {
        int obj_idx = volume->object_idx();
        if (0 <= obj_idx && obj_idx < (int)model->objects.size()) {
            int inst_idx = volume->instance_idx();
            std::vector<Owner>::iterator it = std::find_if(owners.begin(), owners.end(), [obj_idx, inst_idx](const Owner& owner) {
                return (owner.obj_idx == obj_idx) && (owner.inst_idx == inst_idx);
                });
            if (it != owners.end()) {
                it->world_box.merge(volume->transformed_bounding_box());
                it->selected &= volume->selected;
            } else {
                const ModelObject* model_object = model->objects[obj_idx];
                Owner owner;
                owner.obj_idx = obj_idx;
                owner.inst_idx = inst_idx;
                owner.model_instance_id = model_object->instances[inst_idx]->id().id;
                owner.world_box = volume->transformed_bounding_box();
                owner.title = "object" + std::to_string(obj_idx) + "_inst##" + std::to_string(inst_idx);
                owner.label = model_object->name;
                if (model_object->instances.size() > 1)
                    owner.label += " (" + std::to_string(inst_idx + 1) + ")";
                owner.selected = volume->selected;
                owners.emplace_back(owner);
            }
        }
    }

    // updates print order strings
    if (sorted_instances.size() > 1) {
        for (size_t i = 0; i < sorted_instances.size(); ++i) {
            size_t id = sorted_instances[i]->id().id;
            std::vector<Owner>::iterator it = std::find_if(owners.begin(), owners.end(), [id](const Owner& owner) {
                return owner.model_instance_id == id;
                });
            if (it != owners.end())
                it->print_order = std::string((_(L("Seq."))).ToUTF8()) + "#: " + std::to_string(i + 1);
        }
    }

    // calculate eye bounding boxes center zs
    for (Owner& owner : owners) {
        owner.eye_center_z = (world_to_eye * owner.world_box.center())(2);
    }

    // sort owners by center eye zs and selection
    std::sort(owners.begin(), owners.end(), [](const Owner& owner1, const Owner& owner2) {
        if (!owner1.selected && owner2.selected)
            return true;
        else if (owner1.selected && !owner2.selected)
            return false;
        else
            return (owner1.eye_center_z < owner2.eye_center_z);
        });

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    // render info windows
    for (const Owner& owner : owners) {
        Vec3d screen_box_center = world_to_screen * owner.world_box.center();
        float x = 0.0f;
        float y = 0.0f;
        if (camera.get_type() == Camera::Perspective) {
            x = (0.5f + 0.001f * 0.5f * (float)screen_box_center(0)) * viewport[2];
            y = (0.5f - 0.001f * 0.5f * (float)screen_box_center(1)) * viewport[3];
        } else {
            x = (0.5f + 0.5f * (float)screen_box_center(0)) * viewport[2];
            y = (0.5f - 0.5f * (float)screen_box_center(1)) * viewport[3];
        }

        if (x < 0.0f || viewport[2] < x || y < 0.0f || viewport[3] < y)
            continue;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, owner.selected ? 3.0f : 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, owner.selected ? ImVec4(0.757f, 0.404f, 0.216f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
        imgui.set_next_window_pos(x, y, ImGuiCond_Always, 0.5f, 0.5f);
        imgui.begin(owner.title, ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        float win_w = ImGui::GetWindowWidth();
        float label_len = imgui.calc_text_size(owner.label).x;
        ImGui::SetCursorPosX(0.5f * (win_w - label_len));
        ImGui::AlignTextToFramePadding();
        imgui.text(owner.label);

        if (!owner.print_order.empty())
        {
            ImGui::Separator();
            float po_len = imgui.calc_text_size(owner.print_order).x;
            ImGui::SetCursorPosX(0.5f * (win_w - po_len));
            ImGui::AlignTextToFramePadding();
            imgui.text(owner.print_order);
        }

        // force re-render while the windows gets to its final size (it takes several frames)
        if (ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowExpectedSize(ImGui::GetCurrentWindow()).x)
            m_canvas.request_extra_frame();

        imgui.end();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}

#if ENABLE_CANVAS_TOOLTIP_USING_IMGUI
void GLCanvas3D::Tooltip::set_text(const std::string& text)
{
    // If the mouse is inside an ImGUI dialog, then the tooltip is suppressed.
	const std::string &new_text = m_in_imgui ? std::string() : text;
    if (m_text != new_text)
    {
        if (m_text.empty())
            m_start_time = std::chrono::steady_clock::now();

        m_text = new_text;
    }
}

void GLCanvas3D::Tooltip::render(const Vec2d& mouse_position, GLCanvas3D& canvas) const
{
    static ImVec2 size(0.0f, 0.0f);

    auto validate_position = [](const Vec2d& position, const GLCanvas3D& canvas, const ImVec2& wnd_size) {
        Size cnv_size = canvas.get_canvas_size();
        float x = std::clamp((float)position(0), 0.0f, (float)cnv_size.get_width() - wnd_size.x);
        float y = std::clamp((float)position(1) + 16, 0.0f, (float)cnv_size.get_height() - wnd_size.y);
        return Vec2f(x, y);
    };

    if (m_text.empty())
        return;

    // draw the tooltip as hidden until the delay is expired
    // use a value of alpha slightly different from 0.0f because newer imgui does not calculate properly the window size if alpha == 0.0f
    float alpha = (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start_time).count() < 500) ? 0.01f : 1.0f;

    Vec2f position = validate_position(mouse_position, canvas, size);

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    imgui.set_next_window_pos(position(0), position(1), ImGuiCond_Always, 0.0f, 0.0f);

    imgui.begin(_(L("canvas_tooltip")), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::TextUnformatted(m_text.c_str());

    // force re-render while the windows gets to its final size (it may take several frames) or while hidden
    if (alpha < 1.0f || ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowExpectedSize(ImGui::GetCurrentWindow()).x)
        canvas.request_extra_frame();

    size = ImGui::GetWindowSize();

    imgui.end();
    ImGui::PopStyleVar(2);
}
#endif // ENABLE_CANVAS_TOOLTIP_USING_IMGUI

#if ENABLE_SLOPE_RENDERING
void GLCanvas3D::Slope::render() const
{
    if (is_shown())
    {
        const std::array<float, 2>& z_range = m_volumes.get_slope_z_range();
        std::array<float, 2> angle_range = { Geometry::rad2deg(::acos(z_range[0])) - 90.0f, Geometry::rad2deg(::acos(z_range[1])) - 90.0f };
        bool modified = false;

        ImGuiWrapper& imgui = *wxGetApp().imgui();
        const Size& cnv_size = m_canvas.get_canvas_size();
        imgui.set_next_window_pos((float)cnv_size.get_width(), (float)cnv_size.get_height(), ImGuiCond_Always, 1.0f, 1.0f);
        imgui.begin(_(L("Slope visualization")), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        imgui.text(_(L("Facets' normal angle range (degrees)")) + ":");

        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.75f, 0.75f, 0.0f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 0.0f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.85f, 0.85f, 0.0f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.25f, 0.25f, 0.0f, 1.0f));
        if (ImGui::SliderFloat("##yellow", &angle_range[0], 0.0f, 90.0f, "%.1f"))
        {
            modified = true;
            if (angle_range[1] < angle_range[0])
                angle_range[1] = angle_range[0];
        }
        ImGui::PopStyleColor(4);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.75f, 0.0f, 0.0f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 0.0f, 0.0f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.85f, 0.0f, 0.0f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.25f, 0.0f, 0.0f, 1.0f));
        if (ImGui::SliderFloat("##red", &angle_range[1], 0.0f, 90.0f, "%.1f"))
        {
            modified = true;
            if (angle_range[0] > angle_range[1])
                angle_range[0] = angle_range[1];
        }
        ImGui::PopStyleColor(4);

        ImGui::Separator();

        if (imgui.button(_(L("Default"))))
            m_volumes.set_default_slope_z_range();

        // to let the dialog immediately showup without waiting for a mouse move
        if (ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowExpectedSize(ImGui::GetCurrentWindow()).x)
            m_canvas.request_extra_frame();

        imgui.end();

        if (modified)
            m_volumes.set_slope_z_range({ -::cos(Geometry::deg2rad(90.0f - angle_range[0])), -::cos(Geometry::deg2rad(90.0f - angle_range[1])) });
    }
    }
#endif // ENABLE_SLOPE_RENDERING

wxDEFINE_EVENT(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_OBJECT_SELECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RIGHT_CLICK, RBtnEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_REMOVE_OBJECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ARRANGE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SELECT_ALL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_QUESTION_MARK, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INCREASE_INSTANCES, Event<int>);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_MOVED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_ROTATED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_SCALED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_FORCE_UPDATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_WIPETOWER_MOVED, Vec3dEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_WIPETOWER_ROTATED, Vec3dEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, Event<bool>);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE_GEOMETRY, Vec3dsEvent<2>);
wxDEFINE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE_BED_SHAPE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_TAB, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RESETGIZMOS, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_MOVE_DOUBLE_SLIDER, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_EDIT_COLOR_CHANGE, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UNDO, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_REDO, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_COLLAPSE_SIDEBAR, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, Event<float>);
wxDEFINE_EVENT(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, HeightProfileSmoothEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RELOAD_FROM_DISK, SimpleEvent);

const double GLCanvas3D::DefaultCameraZoomToBoxMarginFactor = 1.25;

#if ENABLE_NON_STATIC_CANVAS_MANAGER
GLCanvas3D::GLCanvas3D(wxGLCanvas* canvas)
#else
GLCanvas3D::GLCanvas3D(wxGLCanvas* canvas, Bed3D& bed, Camera& camera, GLToolbar& view_toolbar)
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    : m_canvas(canvas)
    , m_context(nullptr)
#if ENABLE_RETINA_GL
    , m_retina_helper(nullptr)
#endif
    , m_in_render(false)
#if !ENABLE_NON_STATIC_CANVAS_MANAGER
    , m_bed(bed)
    , m_camera(camera)
    , m_view_toolbar(view_toolbar)
#endif // !ENABLE_NON_STATIC_CANVAS_MANAGER
    , m_main_toolbar(GLToolbar::Normal, "Top")
    , m_undoredo_toolbar(GLToolbar::Normal, "Top")
    , m_collapse_toolbar(GLToolbar::Normal, "Top")
    , m_gizmos(*this)
    , m_use_clipping_planes(false)
    , m_sidebar_field("")
    , m_extra_frame_requested(false)
    , m_config(nullptr)
    , m_process(nullptr)
    , m_model(nullptr)
    , m_dirty(true)
    , m_initialized(false)
    , m_apply_zoom_to_volumes_filter(false)
    , m_legend_texture_enabled(false)
    , m_picking_enabled(false)
    , m_moving_enabled(false)
    , m_dynamic_background_enabled(false)
    , m_multisample_allowed(false)
    , m_moving(false)
    , m_tab_down(false)
    , m_cursor_type(Standard)
    , m_color_by("volume")
    , m_reload_delayed(false)
#if ENABLE_RENDER_PICKING_PASS
    , m_show_picking_texture(false)
#endif // ENABLE_RENDER_PICKING_PASS
    , m_render_sla_auxiliaries(true)
    , m_labels(*this)
#if ENABLE_SLOPE_RENDERING
    , m_slope(*this, m_volumes)
#endif // ENABLE_SLOPE_RENDERING
{
    if (m_canvas != nullptr) {
        m_timer.SetOwner(m_canvas);
#if ENABLE_RETINA_GL
        m_retina_helper.reset(new RetinaHelper(canvas));
#if !ENABLE_NON_STATIC_CANVAS_MANAGER
        // set default view_toolbar icons size equal to GLGizmosManager::Default_Icons_Size
        m_view_toolbar.set_icons_size(GLGizmosManager::Default_Icons_Size);
#endif // !ENABLE_NON_STATIC_CANVAS_MANAGER
#endif // ENABLE_RETINA_GL
    }

    m_selection.set_volumes(&m_volumes.volumes);
}

GLCanvas3D::~GLCanvas3D()
{
    reset_volumes();
}

void GLCanvas3D::post_event(wxEvent &&event)
{
    event.SetEventObject(m_canvas);
    wxPostEvent(m_canvas, event);
}

bool GLCanvas3D::init()
{
    if (m_initialized)
        return true;

    if ((m_canvas == nullptr) || (m_context == nullptr))
        return false;

    glsafe(::glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
    glsafe(::glClearDepth(1.0f));

    glsafe(::glDepthFunc(GL_LESS));

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    // Set antialiasing / multisampling
    glsafe(::glDisable(GL_LINE_SMOOTH));
    glsafe(::glDisable(GL_POLYGON_SMOOTH));

    // ambient lighting
    GLfloat ambient[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
    glsafe(::glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient));

    glsafe(::glEnable(GL_LIGHT0));
    glsafe(::glEnable(GL_LIGHT1));

    // light from camera
    GLfloat specular_cam[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
    glsafe(::glLightfv(GL_LIGHT1, GL_SPECULAR, specular_cam));
    GLfloat diffuse_cam[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    glsafe(::glLightfv(GL_LIGHT1, GL_DIFFUSE, diffuse_cam));

    // light from above
    GLfloat specular_top[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    glsafe(::glLightfv(GL_LIGHT0, GL_SPECULAR, specular_top));
    GLfloat diffuse_top[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    glsafe(::glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse_top));

    // Enables Smooth Color Shading; try GL_FLAT for (lack of) fun.
    glsafe(::glShadeModel(GL_SMOOTH));

    // A handy trick -- have surface material mirror the color.
    glsafe(::glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE));
    glsafe(::glEnable(GL_COLOR_MATERIAL));

    if (m_multisample_allowed)
        glsafe(::glEnable(GL_MULTISAMPLE));

    if (!m_shader.init("gouraud.vs", "gouraud.fs"))
    {
        std::cout << "Unable to initialize gouraud shader: please, check that the files gouraud.vs and gouraud.fs are available" << std::endl;
        return false;
    }

    if (m_main_toolbar.is_enabled() && !m_layers_editing.init("variable_layer_height.vs", "variable_layer_height.fs"))
    {
        std::cout << "Unable to initialize variable_layer_height shader: please, check that the files variable_layer_height.vs and variable_layer_height.fs are available" << std::endl;
        return false;
    }

    // on linux the gl context is not valid until the canvas is not shown on screen
    // we defer the geometry finalization of volumes until the first call to render()
    m_volumes.finalize_geometry(true);

    if (m_gizmos.is_enabled() && !m_gizmos.init())
        std::cout << "Unable to initialize gizmos: please, check that all the required textures are available" << std::endl;

    if (!_init_toolbars())
        return false;

    if (m_selection.is_enabled() && !m_selection.init())
        return false;

    m_initialized = true;

    return true;
}

void GLCanvas3D::set_as_dirty()
{
    m_dirty = true;
}

unsigned int GLCanvas3D::get_volumes_count() const
{
    return (unsigned int)m_volumes.volumes.size();
}

void GLCanvas3D::reset_volumes()
{
    if (!m_initialized)
        return;

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    if (m_volumes.empty())
        return;

    _set_current();

    m_selection.clear();
    m_volumes.clear();
    m_dirty = true;

    _set_warning_texture(WarningTexture::ObjectOutside, false);
#else
    _set_current();

    if (!m_volumes.empty())
    {
        m_selection.clear();
        m_volumes.clear();
        m_dirty = true;
    }

    _set_warning_texture(WarningTexture::ObjectOutside, false);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
}

int GLCanvas3D::check_volumes_outside_state() const
{
    ModelInstance::EPrintVolumeState state;
    m_volumes.check_outside_state(m_config, &state);
    return (int)state;
}

void GLCanvas3D::toggle_sla_auxiliaries_visibility(bool visible, const ModelObject* mo, int instance_idx)
{
    m_render_sla_auxiliaries = visible;

    for (GLVolume* vol : m_volumes.volumes) {
        if (vol->composite_id.object_id == 1000)
            continue; // the wipe tower
        if ((mo == nullptr || m_model->objects[vol->composite_id.object_id] == mo)
        && (instance_idx == -1 || vol->composite_id.instance_id == instance_idx)
        && vol->composite_id.volume_id < 0)
            vol->is_active = visible;
    }
}

void GLCanvas3D::toggle_model_objects_visibility(bool visible, const ModelObject* mo, int instance_idx)
{
    for (GLVolume* vol : m_volumes.volumes) {
        if (vol->composite_id.object_id == 1000) { // wipe tower
                vol->is_active = (visible && mo == nullptr);
        }
        else {
            if ((mo == nullptr || m_model->objects[vol->composite_id.object_id] == mo)
            && (instance_idx == -1 || vol->composite_id.instance_id == instance_idx)) {
                vol->is_active = visible;
                vol->force_native_color = (instance_idx != -1);
            }
        }
    }
    if (visible && !mo)
        toggle_sla_auxiliaries_visibility(true, mo, instance_idx);

    if (!mo && !visible && !m_model->objects.empty() && (m_model->objects.size() > 1 || m_model->objects.front()->instances.size() > 1))
        _set_warning_texture(WarningTexture::SomethingNotShown, true);

    if (!mo && visible)
        _set_warning_texture(WarningTexture::SomethingNotShown, false);
}

void GLCanvas3D::update_instance_printable_state_for_object(const size_t obj_idx)
{
    ModelObject* model_object = m_model->objects[obj_idx];
    for (int inst_idx = 0; inst_idx < (int)model_object->instances.size(); ++inst_idx)
    {
        ModelInstance* instance = model_object->instances[inst_idx];

        for (GLVolume* volume : m_volumes.volumes)
        {
            if ((volume->object_idx() == (int)obj_idx) && (volume->instance_idx() == inst_idx))
                volume->printable = instance->printable;
        }
    }
}

void GLCanvas3D::update_instance_printable_state_for_objects(std::vector<size_t>& object_idxs)
{
    for (size_t obj_idx : object_idxs)
        update_instance_printable_state_for_object(obj_idx);
}

void GLCanvas3D::set_config(const DynamicPrintConfig* config)
{
    m_config = config;
    m_layers_editing.set_config(config);
}

void GLCanvas3D::set_process(BackgroundSlicingProcess *process)
{
    m_process = process;
}

void GLCanvas3D::set_model(Model* model)
{
    m_model = model;
    m_selection.set_model(m_model);
}

void GLCanvas3D::bed_shape_changed()
{
    refresh_camera_scene_box();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    wxGetApp().plater()->get_camera().requires_zoom_to_bed = true;
#else
    m_camera.requires_zoom_to_bed = true;
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    m_dirty = true;
}

void GLCanvas3D::set_color_by(const std::string& value)
{
    m_color_by = value;
}

#if ENABLE_NON_STATIC_CANVAS_MANAGER
void GLCanvas3D::refresh_camera_scene_box()
{
    wxGetApp().plater()->get_camera().set_scene_box(scene_bounding_box());
}
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

BoundingBoxf3 GLCanvas3D::volumes_bounding_box() const
{
    BoundingBoxf3 bb;
    for (const GLVolume* volume : m_volumes.volumes)
    {
        if (!m_apply_zoom_to_volumes_filter || ((volume != nullptr) && volume->zoom_to_volumes))
            bb.merge(volume->transformed_bounding_box());
    }
    return bb;
}

BoundingBoxf3 GLCanvas3D::scene_bounding_box() const
{
    BoundingBoxf3 bb = volumes_bounding_box();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    bb.merge(wxGetApp().plater()->get_bed().get_bounding_box(true));
#else
    bb.merge(m_bed.get_bounding_box(true));
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

    if (m_config != nullptr)
    {
        double h = m_config->opt_float("max_print_height");
        bb.min(2) = std::min(bb.min(2), -h);
        bb.max(2) = std::max(bb.max(2), h);
    }

    return bb;
}

bool GLCanvas3D::is_layers_editing_enabled() const
{
    return m_layers_editing.is_enabled();
}

bool GLCanvas3D::is_layers_editing_allowed() const
{
    return m_layers_editing.is_allowed();
}

void GLCanvas3D::reset_layer_height_profile()
{
    wxGetApp().plater()->take_snapshot(_(L("Variable layer height - Reset")));
    m_layers_editing.reset_layer_height_profile(*this);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::adaptive_layer_height_profile(float quality_factor)
{
    wxGetApp().plater()->take_snapshot(_(L("Variable layer height - Adaptive")));
    m_layers_editing.adaptive_layer_height_profile(*this, quality_factor);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::smooth_layer_height_profile(const HeightProfileSmoothingParams& smoothing_params)
{
    wxGetApp().plater()->take_snapshot(_(L("Variable layer height - Smooth all")));
    m_layers_editing.smooth_layer_height_profile(*this, smoothing_params);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

bool GLCanvas3D::is_reload_delayed() const
{
    return m_reload_delayed;
}

void GLCanvas3D::enable_layers_editing(bool enable)
{
#if ENABLE_SLOPE_RENDERING
    if (enable && m_slope.is_shown())
        m_slope.show(false);
#endif // ENABLE_SLOPE_RENDERING

    m_layers_editing.set_enabled(enable);
    const Selection::IndicesList& idxs = m_selection.get_volume_idxs();
    for (unsigned int idx : idxs)
    {
        GLVolume* v = m_volumes.volumes[idx];
        if (v->is_modifier)
            v->force_transparent = enable;
    }

    set_as_dirty();
}

void GLCanvas3D::enable_legend_texture(bool enable)
{
    m_legend_texture_enabled = enable;
}

void GLCanvas3D::enable_picking(bool enable)
{
    m_picking_enabled = enable;
    m_selection.set_mode(Selection::Instance);
}

void GLCanvas3D::enable_moving(bool enable)
{
    m_moving_enabled = enable;
}

void GLCanvas3D::enable_gizmos(bool enable)
{
    m_gizmos.set_enabled(enable);
}

void GLCanvas3D::enable_selection(bool enable)
{
    m_selection.set_enabled(enable);
}

void GLCanvas3D::enable_main_toolbar(bool enable)
{
    m_main_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_undoredo_toolbar(bool enable)
{
    m_undoredo_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_collapse_toolbar(bool enable)
{
    m_collapse_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_dynamic_background(bool enable)
{
    m_dynamic_background_enabled = enable;
}

void GLCanvas3D::allow_multisample(bool allow)
{
    m_multisample_allowed = allow;
}

void GLCanvas3D::zoom_to_bed()
{
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    _zoom_to_box(wxGetApp().plater()->get_bed().get_bounding_box(false));
#else
    _zoom_to_box(m_bed.get_bounding_box(false));
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
}

void GLCanvas3D::zoom_to_volumes()
{
    m_apply_zoom_to_volumes_filter = true;
    _zoom_to_box(volumes_bounding_box());
    m_apply_zoom_to_volumes_filter = false;
}

void GLCanvas3D::zoom_to_selection()
{
    if (!m_selection.is_empty())
        _zoom_to_box(m_selection.get_bounding_box());
}

void GLCanvas3D::select_view(const std::string& direction)
{
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    wxGetApp().plater()->get_camera().select_view(direction);
#else
    m_camera.select_view(direction);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    if (m_canvas != nullptr)
        m_canvas->Refresh();
}

void GLCanvas3D::update_volumes_colors_by_extruder()
{
    if (m_config != nullptr)
        m_volumes.update_colors_by_extruder(m_config);
}

void GLCanvas3D::render()
{
    if (m_in_render)
    {
        // if called recursively, return
        m_dirty = true;
        return;
    }

    m_in_render = true;
    Slic3r::ScopeGuard in_render_guard([this]() { m_in_render = false; });
    (void)in_render_guard;

    if (m_canvas == nullptr)
        return;

    // ensures this canvas is current and initialized
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    if (!_is_shown_on_screen() || !_set_current() || !wxGetApp().init_opengl())
        return;

    if (!is_initialized() && !init())
        return;
#else
    if (! _is_shown_on_screen() || !_set_current() || !_3DScene::init(m_canvas))
        return;
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

#if ENABLE_RENDER_STATISTICS
    auto start_time = std::chrono::high_resolution_clock::now();
#endif // ENABLE_RENDER_STATISTICS

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    if (wxGetApp().plater()->get_bed().get_shape().empty())
#else
    if (m_bed.get_shape().empty())
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    {
        // this happens at startup when no data is still saved under <>\AppData\Roaming\Slic3rPE
        post_event(SimpleEvent(EVT_GLCANVAS_UPDATE_BED_SHAPE));
        return;
    }

    const Size& cnv_size = get_canvas_size();
    // Probably due to different order of events on Linux/GTK2, when one switched from 3D scene
    // to preview, this was called before canvas had its final size. It reported zero width
    // and the viewport was set incorrectly, leading to tripping glAsserts further down
    // the road (in apply_projection). That's why the minimum size is forced to 10.
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    Camera& camera = wxGetApp().plater()->get_camera();
    camera.apply_viewport(0, 0, std::max(10u, (unsigned int)cnv_size.get_width()), std::max(10u, (unsigned int)cnv_size.get_height()));

    if (camera.requires_zoom_to_bed)
    {
        zoom_to_bed();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_bed = false;
    }

    camera.apply_view_matrix();
    camera.apply_projection(_max_bounding_box(true, true));
#else
    m_camera.apply_viewport(0, 0, std::max(10u, (unsigned int)cnv_size.get_width()), std::max(10u, (unsigned int)cnv_size.get_height()));

    if (m_camera.requires_zoom_to_bed)
    {
        zoom_to_bed();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        m_camera.requires_zoom_to_bed = false;
    }

    m_camera.apply_view_matrix();
    m_camera.apply_projection(_max_bounding_box(true, true));
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

    GLfloat position_cam[4] = { 1.0f, 0.0f, 1.0f, 0.0f };
    glsafe(::glLightfv(GL_LIGHT1, GL_POSITION, position_cam));
    GLfloat position_top[4] = { -0.5f, -0.5f, 1.0f, 0.0f };
    glsafe(::glLightfv(GL_LIGHT0, GL_POSITION, position_top));

    wxGetApp().imgui()->new_frame();

    if (m_picking_enabled)
    {
        if (m_rectangle_selection.is_dragging())
            // picking pass using rectangle selection
            _rectangular_selection_picking_pass();
        else
            // regular picking pass
            _picking_pass();
    }

#if ENABLE_RENDER_PICKING_PASS
    if (!m_picking_enabled || !m_show_picking_texture)
    {
#endif // ENABLE_RENDER_PICKING_PASS
    // draw scene
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    _render_background();

    _render_objects();
    _render_sla_slices();
    _render_selection();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    _render_bed(!camera.is_looking_downward(), true);
#else
    _render_bed(!m_camera.is_looking_downward(), true);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

#if ENABLE_RENDER_SELECTION_CENTER
    _render_selection_center();
#endif // ENABLE_RENDER_SELECTION_CENTER

    // we need to set the mouse's scene position here because the depth buffer
    // could be invalidated by the following gizmo render methods
    // this position is used later into on_mouse() to drag the objects
    m_mouse.scene_position = _mouse_to_3d(m_mouse.position.cast<coord_t>());

    _render_current_gizmo();
    _render_selection_sidebar_hints();
#if ENABLE_RENDER_PICKING_PASS
    }
#endif // ENABLE_RENDER_PICKING_PASS

#if ENABLE_SHOW_CAMERA_TARGET
    _render_camera_target();
#endif // ENABLE_SHOW_CAMERA_TARGET

    if (m_picking_enabled && m_rectangle_selection.is_dragging())
        m_rectangle_selection.render(*this);

    // draw overlays
    _render_overlays();

#if ENABLE_RENDER_STATISTICS
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    imgui.begin(std::string("Render statistics"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    imgui.text("Last frame: ");
    ImGui::SameLine();
    imgui.text(std::to_string(m_render_stats.last_frame));
    ImGui::SameLine();
    imgui.text("  ms");
    ImGui::Separator();
    imgui.text("Compressed textures: ");
    ImGui::SameLine();
    imgui.text(GLCanvas3DManager::are_compressed_textures_supported() ? "supported" : "not supported");
    imgui.text("Max texture size: ");
    ImGui::SameLine();
    imgui.text(std::to_string(GLCanvas3DManager::get_gl_info().get_max_tex_size()));
    imgui.end();
#endif // ENABLE_RENDER_STATISTICS

#if ENABLE_CAMERA_STATISTICS
    m_camera.debug_render();
#endif // ENABLE_CAMERA_STATISTICS

#if ENABLE_CANVAS_TOOLTIP_USING_IMGUI
    std::string tooltip;

	// Negative coordinate means out of the window, likely because the window was deactivated.
	// In that case the tooltip should be hidden.
    if (m_mouse.position.x() >= 0. && m_mouse.position.y() >= 0.) 
    {
	    if (tooltip.empty())
	        tooltip = m_layers_editing.get_tooltip(*this);

	    if (tooltip.empty())
	        tooltip = m_gizmos.get_tooltip();

	    if (tooltip.empty())
	        tooltip = m_main_toolbar.get_tooltip();

	    if (tooltip.empty())
	        tooltip = m_undoredo_toolbar.get_tooltip();

	    if (tooltip.empty())
	        tooltip = m_collapse_toolbar.get_tooltip();

	    if (tooltip.empty())
#if ENABLE_NON_STATIC_CANVAS_MANAGER
            tooltip = wxGetApp().plater()->get_view_toolbar().get_tooltip();
#else
            tooltip = m_view_toolbar.get_tooltip();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    }

    set_tooltip(tooltip);

    m_tooltip.render(m_mouse.position, *this);
#endif // ENABLE_CANVAS_TOOLTIP_USING_IMGUI

    wxGetApp().plater()->get_mouse3d_controller().render_settings_dialog(*this);

    wxGetApp().imgui()->render();

    m_canvas->SwapBuffers();

#if ENABLE_RENDER_STATISTICS
    auto end_time = std::chrono::high_resolution_clock::now();
    m_render_stats.last_frame = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
#endif // ENABLE_RENDER_STATISTICS

#if !ENABLE_CANVAS_TOOLTIP_USING_IMGUI
    std::string tooltip = "";

    if (tooltip.empty())
        tooltip = m_layers_editing.get_tooltip(*this);

    if (tooltip.empty())
        tooltip = m_gizmos.get_tooltip();

    if (tooltip.empty())
        tooltip = m_main_toolbar.get_tooltip();

    if (tooltip.empty())
        tooltip = m_undoredo_toolbar.get_tooltip();

    if (tooltip.empty())
        tooltip = m_collapse_toolbar.get_tooltip();

    if (tooltip.empty())
        tooltip = m_view_toolbar.get_tooltip();

    set_tooltip(tooltip);
#endif // !ENABLE_CANVAS_TOOLTIP_USING_IMGUI
}

void GLCanvas3D::render_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, bool printable_only, bool parts_only, bool show_bed, bool transparent_background) const
{
    switch (GLCanvas3DManager::get_framebuffers_type())
    {
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    case GLCanvas3DManager::EFramebufferType::Arb: { _render_thumbnail_framebuffer(thumbnail_data, w, h, printable_only, parts_only, show_bed, transparent_background); break; }
    case GLCanvas3DManager::EFramebufferType::Ext: { _render_thumbnail_framebuffer_ext(thumbnail_data, w, h, printable_only, parts_only, show_bed, transparent_background); break; }
#else
    case GLCanvas3DManager::FB_Arb: { _render_thumbnail_framebuffer(thumbnail_data, w, h, printable_only, parts_only, show_bed, transparent_background); break; }
    case GLCanvas3DManager::FB_Ext: { _render_thumbnail_framebuffer_ext(thumbnail_data, w, h, printable_only, parts_only, show_bed, transparent_background); break; }
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    default: { _render_thumbnail_legacy(thumbnail_data, w, h, printable_only, parts_only, show_bed, transparent_background); break; }
    }
}

void GLCanvas3D::select_all()
{
    m_selection.add_all();
    m_dirty = true;
}

void GLCanvas3D::deselect_all()
{
    m_selection.remove_all();
    wxGetApp().obj_manipul()->set_dirty();
    m_gizmos.reset_all_states();
    m_gizmos.update_data();
    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
}

void GLCanvas3D::delete_selected()
{
    m_selection.erase();
}

void GLCanvas3D::ensure_on_bed(unsigned int object_idx)
{
    typedef std::map<std::pair<int, int>, double> InstancesToZMap;
    InstancesToZMap instances_min_z;

    for (GLVolume* volume : m_volumes.volumes)
    {
        if ((volume->object_idx() == (int)object_idx) && !volume->is_modifier)
        {
            double min_z = volume->transformed_convex_hull_bounding_box().min(2);
            std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::iterator it = instances_min_z.find(instance);
            if (it == instances_min_z.end())
                it = instances_min_z.insert(InstancesToZMap::value_type(instance, DBL_MAX)).first;

            it->second = std::min(it->second, min_z);
        }
    }

    for (GLVolume* volume : m_volumes.volumes)
    {
        std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
        InstancesToZMap::iterator it = instances_min_z.find(instance);
        if (it != instances_min_z.end())
            volume->set_instance_offset(Z, volume->get_instance_offset(Z) - it->second);
    }
}

std::vector<double> GLCanvas3D::get_current_print_zs(bool active_only) const
{
    return m_volumes.get_current_print_zs(active_only);
}

void GLCanvas3D::set_toolpaths_range(double low, double high)
{
    m_volumes.set_range(low, high);
}

std::vector<int> GLCanvas3D::load_object(const ModelObject& model_object, int obj_idx, std::vector<int> instance_idxs)
{
    if (instance_idxs.empty())
    {
        for (unsigned int i = 0; i < model_object.instances.size(); ++i)
        {
            instance_idxs.emplace_back(i);
        }
    }
    return m_volumes.load_object(&model_object, obj_idx, instance_idxs, m_color_by, m_initialized);
}

std::vector<int> GLCanvas3D::load_object(const Model& model, int obj_idx)
{
    if ((0 <= obj_idx) && (obj_idx < (int)model.objects.size()))
    {
        const ModelObject* model_object = model.objects[obj_idx];
        if (model_object != nullptr)
            return load_object(*model_object, obj_idx, std::vector<int>());
    }

    return std::vector<int>();
}

void GLCanvas3D::mirror_selection(Axis axis)
{
    m_selection.mirror(axis);
    do_mirror(L("Mirror Object"));
    wxGetApp().obj_manipul()->set_dirty();
}

// Reload the 3D scene of 
// 1) Model / ModelObjects / ModelInstances / ModelVolumes
// 2) Print bed
// 3) SLA support meshes for their respective ModelObjects / ModelInstances
// 4) Wipe tower preview
// 5) Out of bed collision status & message overlay (texture)
void GLCanvas3D::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if ((m_canvas == nullptr) || (m_config == nullptr) || (m_model == nullptr))
        return;

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    if (!m_initialized)
        return;
    
    _set_current();
#else
    if (m_initialized)
        _set_current();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

    struct ModelVolumeState {
        ModelVolumeState(const GLVolume* volume) :
            model_volume(nullptr), geometry_id(volume->geometry_id), volume_idx(-1) {}
        ModelVolumeState(const ModelVolume* model_volume, const ObjectID& instance_id, const GLVolume::CompositeID& composite_id) :
            model_volume(model_volume), geometry_id(std::make_pair(model_volume->id().id, instance_id.id)), composite_id(composite_id), volume_idx(-1) {}
        ModelVolumeState(const ObjectID& volume_id, const ObjectID& instance_id) :
            model_volume(nullptr), geometry_id(std::make_pair(volume_id.id, instance_id.id)), volume_idx(-1) {}
        bool new_geometry() const { return this->volume_idx == size_t(-1); }
        const ModelVolume* model_volume;
        // ObjectID of ModelVolume + ObjectID of ModelInstance
        // or timestamp of an SLAPrintObjectStep + ObjectID of ModelInstance
        std::pair<size_t, size_t>   geometry_id;
        GLVolume::CompositeID       composite_id;
        // Volume index in the new GLVolume vector.
        size_t                      volume_idx;
    };
    std::vector<ModelVolumeState> model_volume_state;
    std::vector<ModelVolumeState> aux_volume_state;

    struct GLVolumeState {
        GLVolumeState() :
            volume_idx(size_t(-1)) {}
        GLVolumeState(const GLVolume* volume, unsigned int volume_idx) :
            composite_id(volume->composite_id), volume_idx(volume_idx) {}
        GLVolumeState(const GLVolume::CompositeID &composite_id) :
            composite_id(composite_id), volume_idx(size_t(-1)) {}

        GLVolume::CompositeID       composite_id;
        // Volume index in the old GLVolume vector.
        size_t                      volume_idx;
    };

    // SLA steps to pull the preview meshes for.
	typedef std::array<SLAPrintObjectStep, 3> SLASteps;
    SLASteps sla_steps = { slaposDrillHoles, slaposSupportTree, slaposPad };
    struct SLASupportState {
        std::array<PrintStateBase::StateWithTimeStamp, std::tuple_size<SLASteps>::value> step;
    };
    // State of the sla_steps for all SLAPrintObjects.
    std::vector<SLASupportState>   sla_support_state;

    std::vector<size_t> instance_ids_selected;
    std::vector<size_t> map_glvolume_old_to_new(m_volumes.volumes.size(), size_t(-1));
    std::vector<GLVolumeState> deleted_volumes;
    std::vector<GLVolume*> glvolumes_new;
    glvolumes_new.reserve(m_volumes.volumes.size());
    auto model_volume_state_lower = [](const ModelVolumeState& m1, const ModelVolumeState& m2) { return m1.geometry_id < m2.geometry_id; };

    m_reload_delayed = !m_canvas->IsShown() && !refresh_immediately && !force_full_scene_refresh;

    PrinterTechnology printer_technology = m_process->current_printer_technology();
    int               volume_idx_wipe_tower_old = -1;

    // Release invalidated volumes to conserve GPU memory in case of delayed refresh (see m_reload_delayed).
    // First initialize model_volumes_new_sorted & model_instances_new_sorted.
    for (int object_idx = 0; object_idx < (int)m_model->objects.size(); ++object_idx) {
        const ModelObject* model_object = m_model->objects[object_idx];
        for (int instance_idx = 0; instance_idx < (int)model_object->instances.size(); ++instance_idx) {
            const ModelInstance* model_instance = model_object->instances[instance_idx];
            for (int volume_idx = 0; volume_idx < (int)model_object->volumes.size(); ++volume_idx) {
                const ModelVolume* model_volume = model_object->volumes[volume_idx];
                model_volume_state.emplace_back(model_volume, model_instance->id(), GLVolume::CompositeID(object_idx, volume_idx, instance_idx));
            }
        }
    }
    if (printer_technology == ptSLA) {
        const SLAPrint* sla_print = this->sla_print();
#ifndef NDEBUG
        // Verify that the SLAPrint object is synchronized with m_model.
        check_model_ids_equal(*m_model, sla_print->model());
#endif /* NDEBUG */
        sla_support_state.reserve(sla_print->objects().size());
        for (const SLAPrintObject* print_object : sla_print->objects()) {
            SLASupportState state;
            for (size_t istep = 0; istep < sla_steps.size(); ++istep) {
                state.step[istep] = print_object->step_state_with_timestamp(sla_steps[istep]);
                if (state.step[istep].state == PrintStateBase::DONE) {
                    if (!print_object->has_mesh(sla_steps[istep]))
                        // Consider the DONE step without a valid mesh as invalid for the purpose
                        // of mesh visualization.
                        state.step[istep].state = PrintStateBase::INVALID;
                    else if (sla_steps[istep] != slaposDrillHoles)
                        for (const ModelInstance* model_instance : print_object->model_object()->instances)
                            // Only the instances, which are currently printable, will have the SLA support structures kept.
                            // The instances outside the print bed will have the GLVolumes of their support structures released.
                            if (model_instance->is_printable())
                                aux_volume_state.emplace_back(state.step[istep].timestamp, model_instance->id());
                }
            }
            sla_support_state.emplace_back(state);
        }
    }
    std::sort(model_volume_state.begin(), model_volume_state.end(), model_volume_state_lower);
    std::sort(aux_volume_state.begin(), aux_volume_state.end(), model_volume_state_lower);
    // Release all ModelVolume based GLVolumes not found in the current Model. Find the GLVolume of a hollowed mesh.
    for (size_t volume_id = 0; volume_id < m_volumes.volumes.size(); ++volume_id) {
        GLVolume* volume = m_volumes.volumes[volume_id];
        ModelVolumeState  key(volume);
        ModelVolumeState* mvs = nullptr;
        if (volume->volume_idx() < 0) {
            auto it = std::lower_bound(aux_volume_state.begin(), aux_volume_state.end(), key, model_volume_state_lower);
            if (it != aux_volume_state.end() && it->geometry_id == key.geometry_id)
                // This can be an SLA support structure that should not be rendered (in case someone used undo
                // to revert to before it was generated). We only reuse the volume if that's not the case.
                if (m_model->objects[volume->composite_id.object_id]->sla_points_status != sla::PointsStatus::NoPoints)
                    mvs = &(*it);
        }
        else {
            auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
            if (it != model_volume_state.end() && it->geometry_id == key.geometry_id)
                mvs = &(*it);
        }
        // Emplace instance ID of the volume. Both the aux volumes and model volumes share the same instance ID.
        // The wipe tower has its own wipe_tower_instance_id().
        if (m_selection.contains_volume(volume_id))
            instance_ids_selected.emplace_back(volume->geometry_id.second);
        if (mvs == nullptr || force_full_scene_refresh) {
            // This GLVolume will be released.
            if (volume->is_wipe_tower) {
                // There is only one wipe tower.
                assert(volume_idx_wipe_tower_old == -1);
                volume_idx_wipe_tower_old = (int)volume_id;
            }
            if (!m_reload_delayed)
            {
                deleted_volumes.emplace_back(volume, volume_id);
                delete volume;
            }
        }
        else {
            // This GLVolume will be reused.
            volume->set_sla_shift_z(0.0);
            map_glvolume_old_to_new[volume_id] = glvolumes_new.size();
            mvs->volume_idx = glvolumes_new.size();
            glvolumes_new.emplace_back(volume);
            // Update color of the volume based on the current extruder.
            if (mvs->model_volume != nullptr) {
                int extruder_id = mvs->model_volume->extruder_id();
                if (extruder_id != -1)
                    volume->extruder_id = extruder_id;

                volume->is_modifier = !mvs->model_volume->is_model_part();
                volume->set_color_from_model_volume(mvs->model_volume);

                // updates volumes transformations
                volume->set_instance_transformation(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_transformation());
                volume->set_volume_transformation(mvs->model_volume->get_transformation());
            }
        }
    }
    sort_remove_duplicates(instance_ids_selected);
    auto deleted_volumes_lower = [](const GLVolumeState &v1, const GLVolumeState &v2) { return v1.composite_id < v2.composite_id; };
    std::sort(deleted_volumes.begin(), deleted_volumes.end(), deleted_volumes_lower);

    if (m_reload_delayed)
        return;

    bool update_object_list = false;
    if (m_volumes.volumes != glvolumes_new)
		update_object_list = true;
    m_volumes.volumes = std::move(glvolumes_new);
    for (unsigned int obj_idx = 0; obj_idx < (unsigned int)m_model->objects.size(); ++ obj_idx) {
        const ModelObject &model_object = *m_model->objects[obj_idx];
        for (int volume_idx = 0; volume_idx < (int)model_object.volumes.size(); ++ volume_idx) {
			const ModelVolume &model_volume = *model_object.volumes[volume_idx];
            for (int instance_idx = 0; instance_idx < (int)model_object.instances.size(); ++ instance_idx) {
				const ModelInstance &model_instance = *model_object.instances[instance_idx];
				ModelVolumeState key(model_volume.id(), model_instance.id());
				auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
				assert(it != model_volume_state.end() && it->geometry_id == key.geometry_id);
                if (it->new_geometry()) {
                    // New volume.
                    auto it_old_volume = std::lower_bound(deleted_volumes.begin(), deleted_volumes.end(), GLVolumeState(it->composite_id), deleted_volumes_lower);
                    if (it_old_volume != deleted_volumes.end() && it_old_volume->composite_id == it->composite_id)
                        // If a volume changed its ObjectID, but it reuses a GLVolume's CompositeID, maintain its selection.
                        map_glvolume_old_to_new[it_old_volume->volume_idx] = m_volumes.volumes.size();
                    // Note the index of the loaded volume, so that we can reload the main model GLVolume with the hollowed mesh
                    // later in this function.
                    it->volume_idx = m_volumes.volumes.size();
                    m_volumes.load_object_volume(&model_object, obj_idx, volume_idx, instance_idx, m_color_by, m_initialized);
                    m_volumes.volumes.back()->geometry_id = key.geometry_id;
                    update_object_list = true;
                } else {
					// Recycling an old GLVolume.
					GLVolume &existing_volume = *m_volumes.volumes[it->volume_idx];
                    assert(existing_volume.geometry_id == key.geometry_id);
					// Update the Object/Volume/Instance indices into the current Model.
					if (existing_volume.composite_id != it->composite_id) {
						existing_volume.composite_id = it->composite_id;
						update_object_list = true;
					}
                }
            }
        }
    }
    if (printer_technology == ptSLA) {
        size_t idx = 0;
        const SLAPrint *sla_print = this->sla_print();
		std::vector<double> shift_zs(m_model->objects.size(), 0);
        double relative_correction_z = sla_print->relative_correction().z();
        if (relative_correction_z <= EPSILON)
            relative_correction_z = 1.;
		for (const SLAPrintObject *print_object : sla_print->objects()) {
            SLASupportState   &state        = sla_support_state[idx ++];
            const ModelObject *model_object = print_object->model_object();
            // Find an index of the ModelObject
            int object_idx;
            // There may be new SLA volumes added to the scene for this print_object.
            // Find the object index of this print_object in the Model::objects list.
            auto it = std::find(sla_print->model().objects.begin(), sla_print->model().objects.end(), model_object);
            assert(it != sla_print->model().objects.end());
			object_idx = it - sla_print->model().objects.begin();
			// Cache the Z offset to be applied to all volumes with this object_idx.
			shift_zs[object_idx] = print_object->get_current_elevation() / relative_correction_z;
            // Collect indices of this print_object's instances, for which the SLA support meshes are to be added to the scene.
            // pairs of <instance_idx, print_instance_idx>
			std::vector<std::pair<size_t, size_t>> instances[std::tuple_size<SLASteps>::value];
            for (size_t print_instance_idx = 0; print_instance_idx < print_object->instances().size(); ++ print_instance_idx) {
                const SLAPrintObject::Instance &instance = print_object->instances()[print_instance_idx];
                // Find index of ModelInstance corresponding to this SLAPrintObject::Instance.
				auto it = std::find_if(model_object->instances.begin(), model_object->instances.end(), 
                    [&instance](const ModelInstance *mi) { return mi->id() == instance.instance_id; });
                assert(it != model_object->instances.end());
                int instance_idx = it - model_object->instances.begin();
                for (size_t istep = 0; istep < sla_steps.size(); ++ istep)
                    if (sla_steps[istep] == slaposDrillHoles) {
                    	// Hollowing is a special case, where the mesh from the backend is being loaded into the 1st volume of an instance,
                    	// not into its own GLVolume.
                        // There shall always be such a GLVolume allocated.
                        ModelVolumeState key(model_object->volumes.front()->id(), instance.instance_id);
                        auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
                        assert(it != model_volume_state.end() && it->geometry_id == key.geometry_id);
                        assert(!it->new_geometry());
                        GLVolume &volume = *m_volumes.volumes[it->volume_idx];
                        if (! volume.offsets.empty() && state.step[istep].timestamp != volume.offsets.front()) {
                        	// The backend either produced a new hollowed mesh, or it invalidated the one that the front end has seen.
                            volume.indexed_vertex_array.release_geometry();
                        	if (state.step[istep].state == PrintStateBase::DONE) {
                                TriangleMesh mesh = print_object->get_mesh(slaposDrillHoles);
	                            assert(! mesh.empty());
                                mesh.transform(sla_print->sla_trafo(*m_model->objects[volume.object_idx()]).inverse());
                                volume.indexed_vertex_array.load_mesh(mesh);
	                        } else {
	                        	// Reload the original volume.
                                volume.indexed_vertex_array.load_mesh(m_model->objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh());
	                        }
                            volume.finalize_geometry(true);
	                    }
                    	//FIXME it is an ugly hack to write the timestamp into the "offsets" field to not have to add another member variable
                    	// to the GLVolume. We should refactor GLVolume significantly, so that the GLVolume will not contain member variables
                    	// of various concenrs (model vs. 3D print path).
                    	volume.offsets = { state.step[istep].timestamp };
                    } else if (state.step[istep].state == PrintStateBase::DONE) {
                        // Check whether there is an existing auxiliary volume to be updated, or a new auxiliary volume to be created.
						ModelVolumeState key(state.step[istep].timestamp, instance.instance_id.id);
						auto it = std::lower_bound(aux_volume_state.begin(), aux_volume_state.end(), key, model_volume_state_lower);
						assert(it != aux_volume_state.end() && it->geometry_id == key.geometry_id);
                    	if (it->new_geometry()) {
                            // This can be an SLA support structure that should not be rendered (in case someone used undo
                            // to revert to before it was generated). If that's the case, we should not generate anything.
                            if (model_object->sla_points_status != sla::PointsStatus::NoPoints)
                                instances[istep].emplace_back(std::pair<size_t, size_t>(instance_idx, print_instance_idx));
                            else
                                shift_zs[object_idx] = 0.;
                        } else {
                            // Recycling an old GLVolume. Update the Object/Instance indices into the current Model.
                            m_volumes.volumes[it->volume_idx]->composite_id = GLVolume::CompositeID(object_idx, m_volumes.volumes[it->volume_idx]->volume_idx(), instance_idx);
                            m_volumes.volumes[it->volume_idx]->set_instance_transformation(model_object->instances[instance_idx]->get_transformation());
                        }
                    }
            }

            for (size_t istep = 0; istep < sla_steps.size(); ++istep)
                if (!instances[istep].empty())
                    m_volumes.load_object_auxiliary(print_object, object_idx, instances[istep], sla_steps[istep], state.step[istep].timestamp, m_initialized);
        }

		// Shift-up all volumes of the object so that it has the right elevation with respect to the print bed
		for (GLVolume* volume : m_volumes.volumes)
			if (volume->object_idx() < (int)m_model->objects.size() && m_model->objects[volume->object_idx()]->instances[volume->instance_idx()]->is_printable())
				volume->set_sla_shift_z(shift_zs[volume->object_idx()]);
    }

    if (printer_technology == ptFFF && m_config->has("nozzle_diameter"))
    {
        // Should the wipe tower be visualized ?
        unsigned int extruders_count = (unsigned int)dynamic_cast<const ConfigOptionFloats*>(m_config->option("nozzle_diameter"))->values.size();

        bool wt = dynamic_cast<const ConfigOptionBool*>(m_config->option("wipe_tower"))->value;
        bool co = dynamic_cast<const ConfigOptionBool*>(m_config->option("complete_objects"))->value;

        if ((extruders_count > 1) && wt && !co)
        {
            // Height of a print (Show at least a slab)
            double height = std::max(m_model->bounding_box().max(2), 10.0);

            float x = dynamic_cast<const ConfigOptionFloat*>(m_config->option("wipe_tower_x"))->value;
            float y = dynamic_cast<const ConfigOptionFloat*>(m_config->option("wipe_tower_y"))->value;
            float w = dynamic_cast<const ConfigOptionFloat*>(m_config->option("wipe_tower_width"))->value;
            float a = dynamic_cast<const ConfigOptionFloat*>(m_config->option("wipe_tower_rotation_angle"))->value;

            const Print *print = m_process->fff_print();

            const DynamicPrintConfig &print_config  = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            double layer_height                     = print_config.opt_float("layer_height");
            double first_layer_height               = print_config.get_abs_value("first_layer_height", layer_height);
            double nozzle_diameter                  = print->config().nozzle_diameter.values[0];
            float depth = print->wipe_tower_data(extruders_count, first_layer_height, nozzle_diameter).depth;
            float brim_width = print->wipe_tower_data(extruders_count, first_layer_height, nozzle_diameter).brim_width;

            int volume_idx_wipe_tower_new = m_volumes.load_wipe_tower_preview(
                1000, x, y, w, depth, (float)height, a, !print->is_step_done(psWipeTower),
                brim_width, m_initialized);
            if (volume_idx_wipe_tower_old != -1)
                map_glvolume_old_to_new[volume_idx_wipe_tower_old] = volume_idx_wipe_tower_new;
        }
    }

    update_volumes_colors_by_extruder();
	// Update selection indices based on the old/new GLVolumeCollection.
    if (m_selection.get_mode() == Selection::Instance)
        m_selection.instances_changed(instance_ids_selected);
    else
        m_selection.volumes_changed(map_glvolume_old_to_new);

    m_gizmos.update_data();
    m_gizmos.refresh_on_off_state();

    // Update the toolbar
	if (update_object_list)
		post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));

    // checks for geometry outside the print volume to render it accordingly
    if (!m_volumes.empty())
    {
        ModelInstance::EPrintVolumeState state;

        const bool contained_min_one = m_volumes.check_outside_state(m_config, &state);

        _set_warning_texture(WarningTexture::ObjectClashed, state == ModelInstance::PVS_Partly_Outside);
        _set_warning_texture(WarningTexture::ObjectOutside, state == ModelInstance::PVS_Fully_Outside);

        post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, 
                               contained_min_one && !m_model->objects.empty() && state != ModelInstance::PVS_Partly_Outside));
    }
    else
    {
        _set_warning_texture(WarningTexture::ObjectOutside, false);
        _set_warning_texture(WarningTexture::ObjectClashed, false);
        post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, false));
    }

    refresh_camera_scene_box();

    if (m_selection.is_empty())
    {
        // If no object is selected, deactivate the active gizmo, if any
        // Otherwise it may be shown after cleaning the scene (if it was active while the objects were deleted)
        m_gizmos.reset_all_states();

        // If no object is selected, reset the objects manipulator on the sidebar
        // to force a reset of its cache
        auto manip = wxGetApp().obj_manipul();
        if (manip != nullptr)
            manip->set_dirty();
    }

    // and force this canvas to be redrawn.
    m_dirty = true;
}

static void reserve_new_volume_finalize_old_volume(GLVolume& vol_new, GLVolume& vol_old, bool gl_initialized, size_t prealloc_size = VERTEX_BUFFER_RESERVE_SIZE)
{
	// Assign the large pre-allocated buffers to the new GLVolume.
	vol_new.indexed_vertex_array = std::move(vol_old.indexed_vertex_array);
	// Copy the content back to the old GLVolume.
	vol_old.indexed_vertex_array = vol_new.indexed_vertex_array;
	// Clear the buffers, but keep them pre-allocated.
	vol_new.indexed_vertex_array.clear();
	// Just make sure that clear did not clear the reserved memory.
	// Reserving number of vertices (3x position + 3x color)
	vol_new.indexed_vertex_array.reserve(prealloc_size / 6);
	// Finalize the old geometry, possibly move data to the graphics card.
	vol_old.finalize_geometry(gl_initialized);
}

static void load_gcode_retractions(const GCodePreviewData::Retraction& retractions, GLCanvas3D::GCodePreviewVolumeIndex::EType extrusion_type, GLVolumeCollection &volumes, GLCanvas3D::GCodePreviewVolumeIndex &volume_index, bool gl_initialized)
{
	// nothing to render, return
	if (retractions.positions.empty())
		return;

	volume_index.first_volumes.emplace_back(extrusion_type, 0, (unsigned int)volumes.volumes.size());

	GLVolume *volume = volumes.new_nontoolpath_volume(retractions.color.rgba.data(), VERTEX_BUFFER_RESERVE_SIZE);

	GCodePreviewData::Retraction::PositionsList copy(retractions.positions);
	std::sort(copy.begin(), copy.end(), [](const GCodePreviewData::Retraction::Position& p1, const GCodePreviewData::Retraction::Position& p2) { return p1.position(2) < p2.position(2); });

	for (const GCodePreviewData::Retraction::Position& position : copy)
	{
		volume->print_zs.emplace_back(unscale<double>(position.position(2)));
		volume->offsets.emplace_back(volume->indexed_vertex_array.quad_indices.size());
		volume->offsets.emplace_back(volume->indexed_vertex_array.triangle_indices.size());

		_3DScene::point3_to_verts(position.position, position.width, position.height, *volume);

		// Ensure that no volume grows over the limits. If the volume is too large, allocate a new one.
		if (volume->indexed_vertex_array.vertices_and_normals_interleaved.size() > MAX_VERTEX_BUFFER_SIZE) {
			GLVolume &vol = *volume;
			volume = volumes.new_nontoolpath_volume(vol.color);
			reserve_new_volume_finalize_old_volume(*volume, vol, gl_initialized);
		}
	}
	volume->indexed_vertex_array.finalize_geometry(gl_initialized);
}

void GLCanvas3D::load_gcode_preview(const GCodePreviewData& preview_data, const std::vector<std::string>& str_tool_colors)
{
    const Print *print = this->fff_print();
    if ((m_canvas != nullptr) && (print != nullptr))
    {
        _set_current();

        std::vector<float> tool_colors = _parse_colors(str_tool_colors);

        if (m_volumes.empty())
        {
            m_gcode_preview_volume_index.reset();
            
            _load_gcode_extrusion_paths(preview_data, tool_colors);
            _load_gcode_travel_paths(preview_data, tool_colors);
			load_gcode_retractions(preview_data.retraction,   GCodePreviewVolumeIndex::Retraction,   m_volumes, m_gcode_preview_volume_index, m_initialized);
			load_gcode_retractions(preview_data.unretraction, GCodePreviewVolumeIndex::Unretraction, m_volumes, m_gcode_preview_volume_index, m_initialized);
            
            if (!m_volumes.empty())
            {
                // Remove empty volumes from both m_volumes, update m_gcode_preview_volume_index.
                {
	                size_t idx_volume_src = 0;
	                size_t idx_volume_dst = 0;
	                size_t idx_volume_index_src = 0;
	                size_t idx_volume_index_dst = 0;
	                size_t idx_volume_of_this_type_last = (idx_volume_index_src + 1 == m_gcode_preview_volume_index.first_volumes.size()) ? m_volumes.volumes.size() : m_gcode_preview_volume_index.first_volumes[idx_volume_index_src + 1].id;
	                size_t idx_volume_of_this_type_first_new = 0;
	                for (;;) {
	                	if (idx_volume_src == idx_volume_of_this_type_last) {
	                		if (idx_volume_of_this_type_first_new < idx_volume_dst) {
	                			// There are some volumes of this type left, therefore their entry in the index has to be maintained.
	                			if (idx_volume_index_dst < idx_volume_index_src)
	                				m_gcode_preview_volume_index.first_volumes[idx_volume_index_dst] = m_gcode_preview_volume_index.first_volumes[idx_volume_index_src];
	                			m_gcode_preview_volume_index.first_volumes[idx_volume_index_dst].id = idx_volume_of_this_type_first_new;
		                		++ idx_volume_index_dst;
		                	}
	                		if (idx_volume_of_this_type_last == m_volumes.volumes.size())
	                			break;
	                		++ idx_volume_index_src;
	                		idx_volume_of_this_type_last = (idx_volume_index_src + 1 == m_gcode_preview_volume_index.first_volumes.size()) ? m_volumes.volumes.size() : m_gcode_preview_volume_index.first_volumes[idx_volume_index_src + 1].id;
	                		idx_volume_of_this_type_first_new = idx_volume_dst;
	                		if (idx_volume_src == idx_volume_of_this_type_last)
	                			// Empty sequence of volumes for the current index item.
	                			continue;
	                	}
	                	if (! m_volumes.volumes[idx_volume_src]->print_zs.empty())
                			m_volumes.volumes[idx_volume_dst ++] = m_volumes.volumes[idx_volume_src];
	                	++ idx_volume_src;
	                }
	                m_volumes.volumes.erase(m_volumes.volumes.begin() + idx_volume_dst, m_volumes.volumes.end());
	                m_gcode_preview_volume_index.first_volumes.erase(m_gcode_preview_volume_index.first_volumes.begin() + idx_volume_index_dst, m_gcode_preview_volume_index.first_volumes.end());
	            }

                _load_fff_shells();
            }
            _update_toolpath_volumes_outside_state();
        }
        
        _update_gcode_volumes_visibility(preview_data);
        _show_warning_texture_if_needed(WarningTexture::ToolpathOutside);

        if (m_volumes.empty())
            reset_legend_texture();
        else
            _generate_legend_texture(preview_data, tool_colors);
    }
}

void GLCanvas3D::load_sla_preview()
{
    const SLAPrint* print = this->sla_print();
    if ((m_canvas != nullptr) && (print != nullptr))
    {
        _set_current();
	    // Release OpenGL data before generating new data.
	    this->reset_volumes();
        _load_sla_shells();
        _update_sla_shells_outside_state();
        _show_warning_texture_if_needed(WarningTexture::SlaSupportsOutside);
    }
}

void GLCanvas3D::load_preview(const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values)
{
    const Print *print = this->fff_print();
    if (print == nullptr)
        return;

    _set_current();

    // Release OpenGL data before generating new data.
    this->reset_volumes();

    _load_print_toolpaths();
    _load_wipe_tower_toolpaths(str_tool_colors);
    for (const PrintObject* object : print->objects())
        _load_print_object_toolpaths(*object, str_tool_colors, color_print_values);

    _update_toolpath_volumes_outside_state();
    _show_warning_texture_if_needed(WarningTexture::ToolpathOutside);
    if (color_print_values.empty())
        reset_legend_texture();
    else {
        auto preview_data = GCodePreviewData();
        preview_data.extrusion.view_type = GCodePreviewData::Extrusion::ColorPrint;
        const std::vector<float> tool_colors = _parse_colors(str_tool_colors);
        _generate_legend_texture(preview_data, tool_colors);
    }
}

void GLCanvas3D::bind_event_handlers()
{
    if (m_canvas != nullptr)
    {
        m_canvas->Bind(wxEVT_SIZE, &GLCanvas3D::on_size, this);
        m_canvas->Bind(wxEVT_IDLE, &GLCanvas3D::on_idle, this);
        m_canvas->Bind(wxEVT_CHAR, &GLCanvas3D::on_char, this);
        m_canvas->Bind(wxEVT_KEY_DOWN, &GLCanvas3D::on_key, this);
        m_canvas->Bind(wxEVT_KEY_UP, &GLCanvas3D::on_key, this);
        m_canvas->Bind(wxEVT_MOUSEWHEEL, &GLCanvas3D::on_mouse_wheel, this);
        m_canvas->Bind(wxEVT_TIMER, &GLCanvas3D::on_timer, this);
        m_canvas->Bind(wxEVT_LEFT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEFT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MOTION, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_ENTER_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEAVE_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEFT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_PAINT, &GLCanvas3D::on_paint, this);
    }
}

void GLCanvas3D::unbind_event_handlers()
{
    if (m_canvas != nullptr)
    {
        m_canvas->Unbind(wxEVT_SIZE, &GLCanvas3D::on_size, this);
        m_canvas->Unbind(wxEVT_IDLE, &GLCanvas3D::on_idle, this);
        m_canvas->Unbind(wxEVT_CHAR, &GLCanvas3D::on_char, this);
        m_canvas->Unbind(wxEVT_KEY_DOWN, &GLCanvas3D::on_key, this);
        m_canvas->Unbind(wxEVT_KEY_UP, &GLCanvas3D::on_key, this);
        m_canvas->Unbind(wxEVT_MOUSEWHEEL, &GLCanvas3D::on_mouse_wheel, this);
        m_canvas->Unbind(wxEVT_TIMER, &GLCanvas3D::on_timer, this);
        m_canvas->Unbind(wxEVT_LEFT_DOWN, &GLCanvas3D::on_mouse, this);
		m_canvas->Unbind(wxEVT_LEFT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MOTION, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_ENTER_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_LEAVE_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_LEFT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_PAINT, &GLCanvas3D::on_paint, this);
    }
}

void GLCanvas3D::on_size(wxSizeEvent& evt)
{
    m_dirty = true;
}

void GLCanvas3D::on_idle(wxIdleEvent& evt)
{
    if (!m_initialized)
        return;

    m_dirty |= m_main_toolbar.update_items_state();
    m_dirty |= m_undoredo_toolbar.update_items_state();
    m_dirty |= m_collapse_toolbar.update_items_state();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    m_dirty |= wxGetApp().plater()->get_view_toolbar().update_items_state();
    bool mouse3d_controller_applied = wxGetApp().plater()->get_mouse3d_controller().apply(wxGetApp().plater()->get_camera());
#else
    m_dirty |= m_view_toolbar.update_items_state();
    bool mouse3d_controller_applied = wxGetApp().plater()->get_mouse3d_controller().apply(m_camera);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    m_dirty |= mouse3d_controller_applied;

    if (!m_dirty)
        return;

    _refresh_if_shown_on_screen();

    if (m_extra_frame_requested || mouse3d_controller_applied)
    {
        m_dirty = true;
        m_extra_frame_requested = false;
        evt.RequestMore();
    }
    else
        m_dirty = false;
}

void GLCanvas3D::on_char(wxKeyEvent& evt)
{
    if (!m_initialized)
        return;

    // see include/wx/defs.h enum wxKeyCode
    int keyCode = evt.GetKeyCode();
    int ctrlMask = wxMOD_CONTROL;

    auto imgui = wxGetApp().imgui();
    if (imgui->update_key_data(evt)) {
        render();
        return;
    }

    if ((keyCode == WXK_ESCAPE) && (_deactivate_undo_redo_toolbar_items() || _deactivate_search_toolbar_item()))
        return;

    if (m_gizmos.on_char(evt))
        return;

//#ifdef __APPLE__
//    ctrlMask |= wxMOD_RAW_CONTROL;
//#endif /* __APPLE__ */
    if ((evt.GetModifiers() & ctrlMask) != 0) {
        switch (keyCode) {
#ifdef __APPLE__
        case 'a':
        case 'A':
#else /* __APPLE__ */
        case WXK_CONTROL_A:
#endif /* __APPLE__ */
                post_event(SimpleEvent(EVT_GLCANVAS_SELECT_ALL));
        break;
#ifdef __APPLE__
        case 'c':
        case 'C':
#else /* __APPLE__ */
        case WXK_CONTROL_C:
#endif /* __APPLE__ */
            post_event(SimpleEvent(EVT_GLTOOLBAR_COPY));
        break;

#ifdef __linux__
        case WXK_CONTROL_M:
        {
            Mouse3DController& controller = wxGetApp().plater()->get_mouse3d_controller();
            controller.show_settings_dialog(!controller.is_settings_dialog_shown());
            m_dirty = true;
            break;
        }
#endif /* __linux__ */

#ifdef __APPLE__
        case 'v':
        case 'V':
#else /* __APPLE__ */
        case WXK_CONTROL_V:
#endif /* __APPLE__ */
            post_event(SimpleEvent(EVT_GLTOOLBAR_PASTE));
        break;


#ifdef __APPLE__
        case 'f':
        case 'F':
#else /* __APPLE__ */
        case WXK_CONTROL_F:
#endif /* __APPLE__ */
            _activate_search_toolbar_item();
            break;


#ifdef __APPLE__
        case 'y':
        case 'Y':
#else /* __APPLE__ */
        case WXK_CONTROL_Y:
#endif /* __APPLE__ */
            post_event(SimpleEvent(EVT_GLCANVAS_REDO));
        break;
#ifdef __APPLE__
        case 'z':
        case 'Z':
#else /* __APPLE__ */
        case WXK_CONTROL_Z:
#endif /* __APPLE__ */
            post_event(SimpleEvent(EVT_GLCANVAS_UNDO));
        break;

        case WXK_BACK:
        case WXK_DELETE:
             post_event(SimpleEvent(EVT_GLTOOLBAR_DELETE_ALL)); break;
		default:            evt.Skip();
        }
    } else if (evt.HasModifiers()) {
        evt.Skip();
    } else {
        switch (keyCode)
        {
        case WXK_BACK:
		case WXK_DELETE:
                  post_event(SimpleEvent(EVT_GLTOOLBAR_DELETE));
                  break;
        case WXK_ESCAPE: { deselect_all(); break; }
        case WXK_F5: { post_event(SimpleEvent(EVT_GLCANVAS_RELOAD_FROM_DISK)); break; }
        case '0': { select_view("iso"); break; }
        case '1': { select_view("top"); break; }
        case '2': { select_view("bottom"); break; }
        case '3': { select_view("front"); break; }
        case '4': { select_view("rear"); break; }
        case '5': { select_view("left"); break; }
        case '6': { select_view("right"); break; }
        case '+': { 
                    if (dynamic_cast<Preview*>(m_canvas->GetParent()) != nullptr)
                        post_event(wxKeyEvent(EVT_GLCANVAS_EDIT_COLOR_CHANGE, evt)); 
                    else
                        post_event(Event<int>(EVT_GLCANVAS_INCREASE_INSTANCES, +1)); 
                    break; }
        case '-': {  
                    if (dynamic_cast<Preview*>(m_canvas->GetParent()) != nullptr)
                        post_event(wxKeyEvent(EVT_GLCANVAS_EDIT_COLOR_CHANGE, evt)); 
                    else
                        post_event(Event<int>(EVT_GLCANVAS_INCREASE_INSTANCES, -1)); 
                    break; }
        case '?': { post_event(SimpleEvent(EVT_GLCANVAS_QUESTION_MARK)); break; }
        case 'A':
        case 'a': { post_event(SimpleEvent(EVT_GLCANVAS_ARRANGE)); break; }
        case 'B':
        case 'b': { zoom_to_bed(); break; }
#if ENABLE_SLOPE_RENDERING
        case 'D':
        case 'd': {
                    if (!is_layers_editing_enabled())
                    {
                        m_slope.show(!m_slope.is_shown());
                        m_dirty = true;
                    }
                    break;
                  }
#endif // ENABLE_SLOPE_RENDERING
        case 'E':
        case 'e': { m_labels.show(!m_labels.is_shown()); m_dirty = true; break; }
        case 'I':
        case 'i': { _update_camera_zoom(1.0); break; }
        case 'K':
#if ENABLE_NON_STATIC_CANVAS_MANAGER
        case 'k': { wxGetApp().plater()->get_camera().select_next_type(); m_dirty = true; break; }
#else
        case 'k': { m_camera.select_next_type(); m_dirty = true; break; }
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
        case 'O':
        case 'o': { _update_camera_zoom(-1.0); break; }
#if ENABLE_RENDER_PICKING_PASS
        case 'T':
        case 't': {
            m_show_picking_texture = !m_show_picking_texture;
            m_dirty = true; 
            break;
        }
#endif // ENABLE_RENDER_PICKING_PASS
        case 'Z':
        case 'z': { m_selection.is_empty() ? zoom_to_volumes() : zoom_to_selection(); break; }
        default:  { evt.Skip(); break; }
        }
    }
}

class TranslationProcessor
{
    using UpAction = std::function<void(void)>;
    using DownAction = std::function<void(const Vec3d&, bool, bool)>;

    UpAction m_up_action{ nullptr };
    DownAction m_down_action{ nullptr };

    bool m_running{ false };
    Vec3d m_direction{ Vec3d::UnitX() };

public:
    TranslationProcessor(UpAction up_action, DownAction down_action)
        : m_up_action(up_action), m_down_action(down_action)
    {
    }

    void process(wxKeyEvent& evt)
    {
        const int keyCode = evt.GetKeyCode();
        wxEventType type = evt.GetEventType();
        if (type == wxEVT_KEY_UP) {
            switch (keyCode)
            {
            case WXK_NUMPAD_LEFT:  case WXK_LEFT:
            case WXK_NUMPAD_RIGHT: case WXK_RIGHT:
            case WXK_NUMPAD_UP:    case WXK_UP:
            case WXK_NUMPAD_DOWN:  case WXK_DOWN:
            {
                m_running = false;
                m_up_action();
                break;
            }
            default: { break; }
            }
        }
        else if (type == wxEVT_KEY_DOWN) {
            bool apply = false;

            switch (keyCode)
            {
            case WXK_SHIFT:
            {
                if (m_running) 
                    apply = true;

                break;
            }
            case WXK_NUMPAD_LEFT:
            case WXK_LEFT:
            {
                m_direction = -Vec3d::UnitX();
                apply = true;
                break;
            }
            case WXK_NUMPAD_RIGHT:
            case WXK_RIGHT:
            {
                m_direction = Vec3d::UnitX();
                apply = true;
                break;
            }
            case WXK_NUMPAD_UP:
            case WXK_UP:
            {
                m_direction = Vec3d::UnitY();
                apply = true;
                break;
            }
            case WXK_NUMPAD_DOWN:
            case WXK_DOWN:
            {
                m_direction = -Vec3d::UnitY();
                apply = true;
                break;
            }
            default: { break; }
            }

            if (apply) {
                m_running = true;
                m_down_action(m_direction, evt.ShiftDown(), evt.CmdDown());
            }
        }
    }
};

void GLCanvas3D::on_key(wxKeyEvent& evt)
{
    static TranslationProcessor translationProcessor(
        [this]() {
            do_move(L("Gizmo-Move"));
            m_gizmos.update_data();

            wxGetApp().obj_manipul()->set_dirty();
            // Let the plater know that the dragging finished, so a delayed refresh
            // of the scene with the background processing data should be performed.
            post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
            // updates camera target constraints
            refresh_camera_scene_box();
            m_dirty = true;
        },
        [this](const Vec3d& direction, bool slow, bool camera_space) {
            m_selection.start_dragging();
            double multiplier = slow ? 1.0 : 10.0;

            Vec3d displacement;
            if (camera_space)
            {
#if ENABLE_NON_STATIC_CANVAS_MANAGER
                Eigen::Matrix<double, 3, 3, Eigen::DontAlign> inv_view_3x3 = wxGetApp().plater()->get_camera().get_view_matrix().inverse().matrix().block(0, 0, 3, 3);
#else
                Eigen::Matrix<double, 3, 3, Eigen::DontAlign> inv_view_3x3 = m_camera.get_view_matrix().inverse().matrix().block(0, 0, 3, 3);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
                displacement = multiplier * (inv_view_3x3 * direction);
                displacement(2) = 0.0;
            }
            else
                displacement = multiplier * direction;

            m_selection.translate(displacement);
            m_dirty = true;
        }
    );

    const int keyCode = evt.GetKeyCode();

    auto imgui = wxGetApp().imgui();
    if (imgui->update_key_data(evt)) {
        render();
    }
    else
    {
        if (!m_gizmos.on_key(evt))
        {
            if (evt.GetEventType() == wxEVT_KEY_UP) {
                if (m_tab_down && keyCode == WXK_TAB && !evt.HasAnyModifiers()) {
                    // Enable switching between 3D and Preview with Tab
                    // m_canvas->HandleAsNavigationKey(evt);   // XXX: Doesn't work in some cases / on Linux
                    post_event(SimpleEvent(EVT_GLCANVAS_TAB));
                }
                else if (keyCode == WXK_SHIFT)
                {
                    translationProcessor.process(evt);

                    if (m_picking_enabled && m_rectangle_selection.is_dragging())
                    {
                        _update_selection_from_hover();
                        m_rectangle_selection.stop_dragging();
                        m_mouse.ignore_left_up = true;
                        m_dirty = true;
                    }
//                    set_cursor(Standard);
                }
                else if (keyCode == WXK_ALT)
                {
                    if (m_picking_enabled && m_rectangle_selection.is_dragging())
                    {
                        _update_selection_from_hover();
                        m_rectangle_selection.stop_dragging();
                        m_mouse.ignore_left_up = true;
                        m_dirty = true;
                    }
//                    set_cursor(Standard);
                }
                else if (keyCode == WXK_CONTROL)
                    m_dirty = true;
                else if (m_gizmos.is_enabled() && !m_selection.is_empty()) {
                    translationProcessor.process(evt);

                    switch (keyCode)
                    {
                    case WXK_NUMPAD_PAGEUP:   case WXK_PAGEUP:
                    case WXK_NUMPAD_PAGEDOWN: case WXK_PAGEDOWN:
                    {
                        do_rotate(L("Gizmo-Rotate"));
                        m_gizmos.update_data();

                        wxGetApp().obj_manipul()->set_dirty();
                        // Let the plater know that the dragging finished, so a delayed refresh
                        // of the scene with the background processing data should be performed.
                        post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
                        // updates camera target constraints
                        refresh_camera_scene_box();
                        m_dirty = true;

                        break;
                    }
                    default: { break; }
                    }
                }
            }
            else if (evt.GetEventType() == wxEVT_KEY_DOWN) {
                m_tab_down = keyCode == WXK_TAB && !evt.HasAnyModifiers();
                if (keyCode == WXK_SHIFT)
                {
                    translationProcessor.process(evt);

                    if (m_picking_enabled && (m_gizmos.get_current_type() != GLGizmosManager::SlaSupports))
                    {
                        m_mouse.ignore_left_up = false;
//                        set_cursor(Cross);
                    }
                }
                else if (keyCode == WXK_ALT)
                {
                    if (m_picking_enabled && (m_gizmos.get_current_type() != GLGizmosManager::SlaSupports))
                    {
                        m_mouse.ignore_left_up = false;
//                        set_cursor(Cross);
                    }
                }
                else if (keyCode == WXK_CONTROL)
                    m_dirty = true;
                else if (m_gizmos.is_enabled() && !m_selection.is_empty())
                {
                    auto do_rotate = [this](double angle_z_rad) {
                        m_selection.start_dragging();
                        m_selection.rotate(Vec3d(0.0, 0.0, angle_z_rad), TransformationType(TransformationType::World_Relative_Joint));
                        m_dirty = true;
//                        wxGetApp().obj_manipul()->set_dirty();
                    };

                    translationProcessor.process(evt);

                    switch (keyCode)
                    {
                    case WXK_NUMPAD_PAGEUP:   case WXK_PAGEUP:   { do_rotate(0.25 * M_PI); break; }
                    case WXK_NUMPAD_PAGEDOWN: case WXK_PAGEDOWN: { do_rotate(-0.25 * M_PI); break; }
                    default: { break; }
                    }
                }
                else if (!m_gizmos.is_enabled())
                {
                    // DoubleSlider navigation in Preview
                    if (keyCode == WXK_LEFT ||
                        keyCode == WXK_RIGHT ||
                        keyCode == WXK_UP ||
                        keyCode == WXK_DOWN)
                    {
                        if (dynamic_cast<Preview*>(m_canvas->GetParent()) != nullptr)
                            post_event(wxKeyEvent(EVT_GLCANVAS_MOVE_DOUBLE_SLIDER, evt));
                    }
                }
            }
        }
    }

    if (keyCode != WXK_TAB
        && keyCode != WXK_LEFT
        && keyCode != WXK_UP
        && keyCode != WXK_RIGHT
        && keyCode != WXK_DOWN) {
        evt.Skip();   // Needed to have EVT_CHAR generated as well
    }
}

void GLCanvas3D::on_mouse_wheel(wxMouseEvent& evt)
{
#ifdef WIN32
    // Try to filter out spurious mouse wheel events comming from 3D mouse.
    if (wxGetApp().plater()->get_mouse3d_controller().process_mouse_wheel())
        return;
#endif

    if (!m_initialized)
        return;

    // Ignore the wheel events if the middle button is pressed.
    if (evt.MiddleIsDown())
        return;

#if ENABLE_RETINA_GL
    const float scale = m_retina_helper->get_scale_factor();
    evt.SetX(evt.GetX() * scale);
    evt.SetY(evt.GetY() * scale);
#endif

#ifdef __WXMSW__
	// For some reason the Idle event is not being generated after the mouse scroll event in case of scrolling with the two fingers on the touch pad,
	// if the event is not allowed to be passed further.
	// https://github.com/prusa3d/PrusaSlicer/issues/2750
	evt.Skip();
#endif /* __WXMSW__ */

    // Performs layers editing updates, if enabled
    if (is_layers_editing_enabled())
    {
        int object_idx_selected = m_selection.get_object_idx();
        if (object_idx_selected != -1)
        {
            // A volume is selected. Test, whether hovering over a layer thickness bar.
            if (m_layers_editing.bar_rect_contains(*this, (float)evt.GetX(), (float)evt.GetY()))
            {
                // Adjust the width of the selection.
                m_layers_editing.band_width = std::max(std::min(m_layers_editing.band_width * (1.0f + 0.1f * (float)evt.GetWheelRotation() / (float)evt.GetWheelDelta()), 10.0f), 1.5f);
                if (m_canvas != nullptr)
                    m_canvas->Refresh();

                return;
            }
        }
    }

    // If the Search window or Undo/Redo list is opened, 
    // update them according to the event
    if (m_main_toolbar.is_item_pressed("search")    || 
        m_undoredo_toolbar.is_item_pressed("undo")  || 
        m_undoredo_toolbar.is_item_pressed("redo")) {
        m_mouse_wheel = int((double)evt.GetWheelRotation() / (double)evt.GetWheelDelta());
        return;
    }

    // Inform gizmos about the event so they have the opportunity to react.
    if (m_gizmos.on_mouse_wheel(evt))
        return;

    // Calculate the zoom delta and apply it to the current zoom factor
    _update_camera_zoom((double)evt.GetWheelRotation() / (double)evt.GetWheelDelta());
}

void GLCanvas3D::on_timer(wxTimerEvent& evt)
{
    if (m_layers_editing.state == LayersEditing::Editing)
        _perform_layer_editing_action();
}

#ifndef NDEBUG
// #define SLIC3R_DEBUG_MOUSE_EVENTS
#endif

#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
std::string format_mouse_event_debug_message(const wxMouseEvent &evt)
{
	static int idx = 0;
	char buf[2048];
	std::string out;
	sprintf(buf, "Mouse Event %d - ", idx ++);
	out = buf;

	if (evt.Entering())
		out += "Entering ";
	if (evt.Leaving())
		out += "Leaving ";
	if (evt.Dragging())
		out += "Dragging ";
	if (evt.Moving())
		out += "Moving ";
	if (evt.Magnify())
		out += "Magnify ";
	if (evt.LeftDown())
		out += "LeftDown ";
	if (evt.LeftUp())
		out += "LeftUp ";
	if (evt.LeftDClick())
		out += "LeftDClick ";
	if (evt.MiddleDown())
		out += "MiddleDown ";
	if (evt.MiddleUp())
		out += "MiddleUp ";
	if (evt.MiddleDClick())
		out += "MiddleDClick ";
	if (evt.RightDown())
		out += "RightDown ";
	if (evt.RightUp())
		out += "RightUp ";
	if (evt.RightDClick())
		out += "RightDClick ";

	sprintf(buf, "(%d, %d)", evt.GetX(), evt.GetY());
	out += buf;
	return out;
}
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */

void GLCanvas3D::on_mouse(wxMouseEvent& evt)
{
    if (!m_initialized || !_set_current())
        return;

#if ENABLE_RETINA_GL
    const float scale = m_retina_helper->get_scale_factor();
    evt.SetX(evt.GetX() * scale);
    evt.SetY(evt.GetY() * scale);
#endif

    Point pos(evt.GetX(), evt.GetY());

    ImGuiWrapper* imgui = wxGetApp().imgui();
#if ENABLE_CANVAS_TOOLTIP_USING_IMGUI
    if (m_tooltip.is_in_imgui() && evt.LeftUp())
        // ignore left up events coming from imgui windows and not processed by them
        m_mouse.ignore_left_up = true;
    m_tooltip.set_in_imgui(false);
#endif // ENABLE_CANVAS_TOOLTIP_USING_IMGUI
    if (imgui->update_mouse_data(evt)) {
        m_mouse.position = evt.Leaving() ? Vec2d(-1.0, -1.0) : pos.cast<double>();
#if ENABLE_CANVAS_TOOLTIP_USING_IMGUI
        m_tooltip.set_in_imgui(true);
#endif // ENABLE_CANVAS_TOOLTIP_USING_IMGUI
        render();
#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
        printf((format_mouse_event_debug_message(evt) + " - Consumed by ImGUI\n").c_str());
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */
        // do not return if dragging or tooltip not empty to allow for tooltip update
#if ENABLE_CANVAS_TOOLTIP_USING_IMGUI
        if (!m_mouse.dragging && m_tooltip.is_empty())
            return;
#else
        if (!m_mouse.dragging && m_canvas->GetToolTipText().empty())
            return;
#endif // ENABLE_CANVAS_TOOLTIP_USING_IMGUI
    }

#ifdef __WXMSW__
	bool on_enter_workaround = false;
    if (! evt.Entering() && ! evt.Leaving() && m_mouse.position.x() == -1.0) {
        // Workaround for SPE-832: There seems to be a mouse event sent to the window before evt.Entering()
        m_mouse.position = pos.cast<double>();
        render();
#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
		printf((format_mouse_event_debug_message(evt) + " - OnEnter workaround\n").c_str());
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */
		on_enter_workaround = true;
    } else 
#endif /* __WXMSW__ */
    {
#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
		printf((format_mouse_event_debug_message(evt) + " - other\n").c_str());
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */
	}

    if (m_main_toolbar.on_mouse(evt, *this))
    {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }

    if (m_undoredo_toolbar.on_mouse(evt, *this))
    {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }

    if (m_collapse_toolbar.on_mouse(evt, *this))
    {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    if (wxGetApp().plater()->get_view_toolbar().on_mouse(evt, *this))
#else
    if (m_view_toolbar.on_mouse(evt, *this))
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }

    if (m_gizmos.on_mouse(evt))
    {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();

        m_mouse.set_start_position_3D_as_invalid();
#if ENABLE_CANVAS_TOOLTIP_USING_IMGUI
        m_mouse.position = pos.cast<double>();
#endif /// ENABLE_CANVAS_TOOLTIP_USING_IMGUI
        return;
    }

    int selected_object_idx = m_selection.get_object_idx();
    int layer_editing_object_idx = is_layers_editing_enabled() ? selected_object_idx : -1;
    m_layers_editing.select_object(*m_model, layer_editing_object_idx);

    if (m_mouse.drag.move_requires_threshold && m_mouse.is_move_start_threshold_position_2D_defined() && m_mouse.is_move_threshold_met(pos))
    {
        m_mouse.drag.move_requires_threshold = false;
        m_mouse.set_move_start_threshold_position_2D_as_invalid();
    }

    if (evt.ButtonDown() && wxWindow::FindFocus() != this->m_canvas)
        // Grab keyboard focus on any mouse click event.
        m_canvas->SetFocus();

    if (evt.Entering())
    {
//#if defined(__WXMSW__) || defined(__linux__)
//        // On Windows and Linux needs focus in order to catch key events
        // Set focus in order to remove it from sidebar fields
        if (m_canvas != nullptr) {
            // Only set focus, if the top level window of this canvas is active.
            auto p = dynamic_cast<wxWindow*>(evt.GetEventObject());
            while (p->GetParent())
                p = p->GetParent();
            auto *top_level_wnd = dynamic_cast<wxTopLevelWindow*>(p);
            if (top_level_wnd && top_level_wnd->IsActive())
                m_canvas->SetFocus();
            m_mouse.position = pos.cast<double>();
            // 1) forces a frame render to ensure that m_hover_volume_idxs is updated even when the user right clicks while
            // the context menu is shown, ensuring it to disappear if the mouse is outside any volume and to
            // change the volume hover state if any is under the mouse 
            // 2) when switching between 3d view and preview the size of the canvas changes if the side panels are visible,
            // so forces a resize to avoid multiple renders with different sizes (seen as flickering)
            _refresh_if_shown_on_screen();
        }
        m_mouse.set_start_position_2D_as_invalid();
//#endif
    }
    else if (evt.Leaving())
    {
        _deactivate_undo_redo_toolbar_items();
        _deactivate_search_toolbar_item();

        // to remove hover on objects when the mouse goes out of this canvas
        m_mouse.position = Vec2d(-1.0, -1.0);
        m_dirty = true;
    }
    else if (evt.LeftDown() || evt.RightDown() || evt.MiddleDown())
    {
        if (_deactivate_undo_redo_toolbar_items() || _deactivate_search_toolbar_item())
            return;

        // If user pressed left or right button we first check whether this happened
        // on a volume or not.
        m_layers_editing.state = LayersEditing::Unknown;
        if ((layer_editing_object_idx != -1) && m_layers_editing.bar_rect_contains(*this, pos(0), pos(1)))
        {
            // A volume is selected and the mouse is inside the layer thickness bar.
            // Start editing the layer height.
            m_layers_editing.state = LayersEditing::Editing;
            _perform_layer_editing_action(&evt);
        }
        else if (evt.LeftDown() && (evt.ShiftDown() || evt.AltDown()) && m_picking_enabled)
        {
            if (m_gizmos.get_current_type() != GLGizmosManager::SlaSupports)
            {
                m_rectangle_selection.start_dragging(m_mouse.position, evt.ShiftDown() ? GLSelectionRectangle::Select : GLSelectionRectangle::Deselect);
                m_dirty = true;
            }
        }
        else
        {
            // Select volume in this 3D canvas.
            // Don't deselect a volume if layer editing is enabled. We want the object to stay selected
            // during the scene manipulation.

            if (m_picking_enabled && (!m_hover_volume_idxs.empty() || !is_layers_editing_enabled()))
            {
                if (evt.LeftDown() && !m_hover_volume_idxs.empty())
                {
                    int volume_idx = get_first_hover_volume_idx();
                    bool already_selected = m_selection.contains_volume(volume_idx);
                    bool ctrl_down = evt.CmdDown();

                    Selection::IndicesList curr_idxs = m_selection.get_volume_idxs();

                    if (already_selected && ctrl_down)
                        m_selection.remove(volume_idx);
                    else
                    {
                        m_selection.add(volume_idx, !ctrl_down, true);
                        m_mouse.drag.move_requires_threshold = !already_selected;
                        if (already_selected)
                            m_mouse.set_move_start_threshold_position_2D_as_invalid();
                        else
                            m_mouse.drag.move_start_threshold_position_2D = pos;
                    }

                    // propagate event through callback
                    if (curr_idxs != m_selection.get_volume_idxs())
                    {
                        if (m_selection.is_empty())
                            m_gizmos.reset_all_states();
                        else
                            m_gizmos.refresh_on_off_state();

                        m_gizmos.update_data();
                        post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
                        m_dirty = true;
                    }
                }
            }

            if (!m_hover_volume_idxs.empty())
            {
                if (evt.LeftDown() && m_moving_enabled && (m_mouse.drag.move_volume_idx == -1))
                {
                    // Only accept the initial position, if it is inside the volume bounding box.
                    int volume_idx = get_first_hover_volume_idx();
                    BoundingBoxf3 volume_bbox = m_volumes.volumes[volume_idx]->transformed_bounding_box();
                    volume_bbox.offset(1.0);
                    if (volume_bbox.contains(m_mouse.scene_position))
                    {
                        m_volumes.volumes[volume_idx]->hover = GLVolume::HS_None;
                        // The dragging operation is initiated.
                        m_mouse.drag.move_volume_idx = volume_idx;
                        m_selection.start_dragging();
                        m_mouse.drag.start_position_3D = m_mouse.scene_position;
                        m_moving = true;
                    }
                }
            }
        }
    }
    else if (evt.Dragging() && evt.LeftIsDown() && (m_layers_editing.state == LayersEditing::Unknown) && (m_mouse.drag.move_volume_idx != -1))
    {
        if (!m_mouse.drag.move_requires_threshold)
        {
            m_mouse.dragging = true;

            Vec3d cur_pos = m_mouse.drag.start_position_3D;
            // we do not want to translate objects if the user just clicked on an object while pressing shift to remove it from the selection and then drag
            if (m_selection.contains_volume(get_first_hover_volume_idx()))
            {
#if ENABLE_NON_STATIC_CANVAS_MANAGER
                const Camera& camera = wxGetApp().plater()->get_camera();
                if (std::abs(camera.get_dir_forward()(2)) < EPSILON)
#else
                if (std::abs(m_camera.get_dir_forward()(2)) < EPSILON)
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
                {
                    // side view -> move selected volumes orthogonally to camera view direction
                    Linef3 ray = mouse_ray(pos);
                    Vec3d dir = ray.unit_vector();
                    // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing throught the starting position
                    // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebric form
                    // in our case plane normal and ray direction are the same (orthogonal view)
                    // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
                    Vec3d inters = ray.a + (m_mouse.drag.start_position_3D - ray.a).dot(dir) / dir.squaredNorm() * dir;
                    // vector from the starting position to the found intersection
                    Vec3d inters_vec = inters - m_mouse.drag.start_position_3D;

#if ENABLE_NON_STATIC_CANVAS_MANAGER
                    Vec3d camera_right = camera.get_dir_right();
                    Vec3d camera_up = camera.get_dir_up();
#else
                    Vec3d camera_right = m_camera.get_dir_right();
                    Vec3d camera_up = m_camera.get_dir_up();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

                    // finds projection of the vector along the camera axes
                    double projection_x = inters_vec.dot(camera_right);
                    double projection_z = inters_vec.dot(camera_up);

                    // apply offset
                    cur_pos = m_mouse.drag.start_position_3D + projection_x * camera_right + projection_z * camera_up;
                }
                else
                {
                    // Generic view
                    // Get new position at the same Z of the initial click point.
                    float z0 = 0.0f;
                    float z1 = 1.0f;
                    cur_pos = Linef3(_mouse_to_3d(pos, &z0), _mouse_to_3d(pos, &z1)).intersect_plane(m_mouse.drag.start_position_3D(2));
                }
            }

            m_selection.translate(cur_pos - m_mouse.drag.start_position_3D);
            wxGetApp().obj_manipul()->set_dirty();
            m_dirty = true;
        }
    }
    else if (evt.Dragging() && evt.LeftIsDown() && m_picking_enabled && m_rectangle_selection.is_dragging())
    {
        m_rectangle_selection.dragging(pos.cast<double>());
        m_dirty = true;
    }
    else if (evt.Dragging())
    {
        m_mouse.dragging = true;

        if ((m_layers_editing.state != LayersEditing::Unknown) && (layer_editing_object_idx != -1))
        {
            if (m_layers_editing.state == LayersEditing::Editing)
            {
                _perform_layer_editing_action(&evt);
                m_mouse.position = pos.cast<double>();
            }
        }
        // do not process the dragging if the left mouse was set down in another canvas
        else if (evt.LeftIsDown())
        {
            // if dragging over blank area with left button, rotate
            if (m_hover_volume_idxs.empty() && m_mouse.is_start_position_3D_defined())
            {
                const Vec3d rot = (Vec3d(pos.x(), pos.y(), 0.) - m_mouse.drag.start_position_3D) * (PI * TRACKBALLSIZE / 180.);
                if (wxGetApp().app_config->get("use_free_camera") == "1")
                    // Virtual track ball (similar to the 3DConnexion mouse).
#if ENABLE_NON_STATIC_CANVAS_MANAGER
                    wxGetApp().plater()->get_camera().rotate_local_around_target(Vec3d(rot.y(), rot.x(), 0.));
#else
                    m_camera.rotate_local_around_target(Vec3d(rot.y(), rot.x(), 0.));
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
                else
                {
                    // Forces camera right vector to be parallel to XY plane in case it has been misaligned using the 3D mouse free rotation.
                    // It is cheaper to call this function right away instead of testing wxGetApp().plater()->get_mouse3d_controller().connected(),
                    // which checks an atomics (flushes CPU caches).
                    // See GH issue #3816.
#if ENABLE_NON_STATIC_CANVAS_MANAGER
                    Camera& camera = wxGetApp().plater()->get_camera();
                    camera.recover_from_free_camera();
                    camera.rotate_on_sphere(rot.x(), rot.y(), wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptSLA);
#else
                    m_camera.recover_from_free_camera();
                    m_camera.rotate_on_sphere(rot.x(), rot.y(), wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptSLA);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
                }

                m_dirty = true;
            }
            m_mouse.drag.start_position_3D = Vec3d((double)pos(0), (double)pos(1), 0.0);
        }
        else if (evt.MiddleIsDown() || evt.RightIsDown())
        {
            // If dragging over blank area with right button, pan.
            if (m_mouse.is_start_position_2D_defined())
            {
                // get point in model space at Z = 0
                float z = 0.0f;
                const Vec3d& cur_pos = _mouse_to_3d(pos, &z);
                Vec3d orig = _mouse_to_3d(m_mouse.drag.start_position_2D, &z);
#if ENABLE_NON_STATIC_CANVAS_MANAGER
                Camera& camera = wxGetApp().plater()->get_camera();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER                
                if (wxGetApp().app_config->get("use_free_camera") != "1")
                    // Forces camera right vector to be parallel to XY plane in case it has been misaligned using the 3D mouse free rotation.
                    // It is cheaper to call this function right away instead of testing wxGetApp().plater()->get_mouse3d_controller().connected(),
                    // which checks an atomics (flushes CPU caches).
                    // See GH issue #3816.
#if ENABLE_NON_STATIC_CANVAS_MANAGER
                    camera.recover_from_free_camera();

                camera.set_target(camera.get_target() + orig - cur_pos);
#else
                    m_camera.recover_from_free_camera();

                m_camera.set_target(m_camera.get_target() + orig - cur_pos);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER                
                m_dirty = true;
            }

            m_mouse.drag.start_position_2D = pos;
        }
    }
    else if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
    {
        if (m_layers_editing.state != LayersEditing::Unknown)
        {
            m_layers_editing.state = LayersEditing::Unknown;
            _stop_timer();
            m_layers_editing.accept_changes(*this);
        }
        else if ((m_mouse.drag.move_volume_idx != -1) && m_mouse.dragging)
        {
            do_move(L("Move Object"));
            wxGetApp().obj_manipul()->set_dirty();
            // Let the plater know that the dragging finished, so a delayed refresh
            // of the scene with the background processing data should be performed.
            post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
        }
        else if (evt.LeftUp() && m_picking_enabled && m_rectangle_selection.is_dragging())
        {
            if (evt.ShiftDown() || evt.AltDown())
                _update_selection_from_hover();

            m_rectangle_selection.stop_dragging();
        }
        else if (evt.LeftUp() && !m_mouse.ignore_left_up && !m_mouse.dragging && m_hover_volume_idxs.empty() && !is_layers_editing_enabled())
        {
            // deselect and propagate event through callback
            if (!evt.ShiftDown() && m_picking_enabled)
                deselect_all();
        }
        else if (evt.RightUp())
        {
            m_mouse.position = pos.cast<double>();
            // forces a frame render to ensure that m_hover_volume_idxs is updated even when the user right clicks while
            // the context menu is already shown
            render();
            if (!m_hover_volume_idxs.empty())
            {
                // if right clicking on volume, propagate event through callback (shows context menu)
                int volume_idx = get_first_hover_volume_idx();
                if (!m_volumes.volumes[volume_idx]->is_wipe_tower // no context menu for the wipe tower
                    && m_gizmos.get_current_type() != GLGizmosManager::SlaSupports)  // disable context menu when the gizmo is open
                {
                    // forces the selection of the volume
                    /* m_selection.add(volume_idx); // #et_FIXME_if_needed
                     * To avoid extra "Add-Selection" snapshots,
                     * call add() with check_for_already_contained=true
                     * */
                    m_selection.add(volume_idx, true, true); 
                    m_gizmos.refresh_on_off_state();
                    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
                    m_gizmos.update_data();
                    wxGetApp().obj_manipul()->set_dirty();
                    // forces a frame render to update the view before the context menu is shown
                    render();
                }
            }
            Vec2d logical_pos = pos.cast<double>();
#if ENABLE_RETINA_GL
            const float factor = m_retina_helper->get_scale_factor();
            logical_pos = logical_pos.cwiseQuotient(Vec2d(factor, factor));
#endif // ENABLE_RETINA_GL
            if (!m_mouse.dragging)
                // do not post the event if the user is panning the scene
                post_event(RBtnEvent(EVT_GLCANVAS_RIGHT_CLICK, { logical_pos, m_hover_volume_idxs.empty() }));
        }

        mouse_up_cleanup();
    }
    else if (evt.Moving())
    {
        m_mouse.position = pos.cast<double>();

        // updates gizmos overlay
        if (m_selection.is_empty())
            m_gizmos.reset_all_states();

        // Only refresh if picking is enabled, in that case the objects may get highlighted if the mouse cursor hovers over.
        if (m_picking_enabled)
            m_dirty = true;
    }
    else
        evt.Skip();

#ifdef __WXMSW__
	if (on_enter_workaround)
		m_mouse.position = Vec2d(-1., -1.);
#endif /* __WXMSW__ */
}

void GLCanvas3D::on_paint(wxPaintEvent& evt)
{
    if (m_initialized)
        m_dirty = true;
    else
        // Call render directly, so it gets initialized immediately, not from On Idle handler.
        this->render();
}

Size GLCanvas3D::get_canvas_size() const
{
    int w = 0;
    int h = 0;

    if (m_canvas != nullptr)
        m_canvas->GetSize(&w, &h);

#if ENABLE_RETINA_GL
    const float factor = m_retina_helper->get_scale_factor();
    w *= factor;
    h *= factor;
#else
    const float factor = 1.0f;
#endif

    return Size(w, h, factor);
}

Vec2d GLCanvas3D::get_local_mouse_position() const
{
    if (m_canvas == nullptr)
		return Vec2d::Zero();

    wxPoint mouse_pos = m_canvas->ScreenToClient(wxGetMousePosition());
    const double factor = 
#if ENABLE_RETINA_GL
        m_retina_helper->get_scale_factor();
#else
        1.0;
#endif
    return Vec2d(factor * mouse_pos.x, factor * mouse_pos.y);
}

void GLCanvas3D::reset_legend_texture()
{
    if (m_legend_texture.get_id() != 0)
    {
        _set_current();
        m_legend_texture.reset();
    }
}

void GLCanvas3D::set_tooltip(const std::string& tooltip) const
{
    if (m_canvas != nullptr)
    {
#if ENABLE_CANVAS_TOOLTIP_USING_IMGUI
        m_tooltip.set_text(tooltip);
#else
        wxString txt = wxString::FromUTF8(tooltip.data());
        if (m_canvas->GetToolTipText() != txt)
            m_canvas->SetToolTip(txt);

//        wxToolTip* t = m_canvas->GetToolTip();
//        if (t != nullptr)
//        {
//            if (tooltip.empty())
//                m_canvas->UnsetToolTip();
//            else
//                t->SetTip(wxString::FromUTF8(tooltip.data()));
//        }
//        else if (!tooltip.empty()) // Avoid "empty" tooltips => unset of the empty tooltip leads to application crash under OSX
//            m_canvas->SetToolTip(wxString::FromUTF8(tooltip.data()));
#endif // ENABLE_CANVAS_TOOLTIP_USING_IMGUI
    }
}

void GLCanvas3D::do_move(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(_(snapshot_type));

    std::set<std::pair<int, int>> done;  // keeps track of modified instances
    bool object_moved = false;
    Vec3d wipe_tower_origin = Vec3d::Zero();

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes)
    {
        int object_idx = v->object_idx();
        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        std::pair<int, int> done_id(object_idx, instance_idx);

        if ((0 <= object_idx) && (object_idx < (int)m_model->objects.size()))
        {
            done.insert(done_id);

            // Move instances/volumes
            ModelObject* model_object = m_model->objects[object_idx];
            if (model_object != nullptr)
            {
                if (selection_mode == Selection::Instance)
                    model_object->instances[instance_idx]->set_offset(v->get_instance_offset());
                else if (selection_mode == Selection::Volume)
                    model_object->volumes[volume_idx]->set_offset(v->get_volume_offset());

                object_moved = true;
                model_object->invalidate_bounding_box();
            }
        }
        else if (object_idx == 1000)
            // Move a wipe tower proxy.
            wipe_tower_origin = v->get_volume_offset();
    }

    // Fixes sinking/flying instances
    for (const std::pair<int, int>& i : done)
    {
        ModelObject* m = m_model->objects[i.first];
        Vec3d shift(0.0, 0.0, -m->get_instance_min_z(i.second));
        m_selection.translate(i.first, i.second, shift);
        m->translate_instance(i.second, shift);
    }

    if (object_moved)
        post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_MOVED));

    if (wipe_tower_origin != Vec3d::Zero())
        post_event(Vec3dEvent(EVT_GLCANVAS_WIPETOWER_MOVED, std::move(wipe_tower_origin)));

    m_dirty = true;
}

void GLCanvas3D::do_rotate(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(_(snapshot_type));

    std::set<std::pair<int, int>> done;  // keeps track of modified instances

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes)
    {
        int object_idx = v->object_idx();
        if (object_idx == 1000) { // the wipe tower
            Vec3d offset = v->get_volume_offset();
            post_event(Vec3dEvent(EVT_GLCANVAS_WIPETOWER_ROTATED, Vec3d(offset(0), offset(1), v->get_volume_rotation()(2))));
        }
        if ((object_idx < 0) || ((int)m_model->objects.size() <= object_idx))
            continue;

        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        done.insert(std::pair<int, int>(object_idx, instance_idx));

        // Rotate instances/volumes.
        ModelObject* model_object = m_model->objects[object_idx];
        if (model_object != nullptr)
        {
            if (selection_mode == Selection::Instance)
            {
                model_object->instances[instance_idx]->set_rotation(v->get_instance_rotation());
                model_object->instances[instance_idx]->set_offset(v->get_instance_offset());
            }
            else if (selection_mode == Selection::Volume)
            {
                model_object->volumes[volume_idx]->set_rotation(v->get_volume_rotation());
                model_object->volumes[volume_idx]->set_offset(v->get_volume_offset());
            }
            model_object->invalidate_bounding_box();
        }
    }

    // Fixes sinking/flying instances
    for (const std::pair<int, int>& i : done)
    {
        ModelObject* m = m_model->objects[i.first];
        Vec3d shift(0.0, 0.0, -m->get_instance_min_z(i.second));
        m_selection.translate(i.first, i.second, shift);
        m->translate_instance(i.second, shift);
    }

    if (!done.empty())
        post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_ROTATED));

    m_dirty = true;
}

void GLCanvas3D::do_scale(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(_(snapshot_type));

    std::set<std::pair<int, int>> done;  // keeps track of modified instances

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes)
    {
        int object_idx = v->object_idx();
        if ((object_idx < 0) || ((int)m_model->objects.size() <= object_idx))
            continue;

        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        done.insert(std::pair<int, int>(object_idx, instance_idx));

        // Rotate instances/volumes
        ModelObject* model_object = m_model->objects[object_idx];
        if (model_object != nullptr)
        {
            if (selection_mode == Selection::Instance)
            {
                model_object->instances[instance_idx]->set_scaling_factor(v->get_instance_scaling_factor());
                model_object->instances[instance_idx]->set_offset(v->get_instance_offset());
            }
            else if (selection_mode == Selection::Volume)
            {
                model_object->instances[instance_idx]->set_offset(v->get_instance_offset());
                model_object->volumes[volume_idx]->set_scaling_factor(v->get_volume_scaling_factor());
                model_object->volumes[volume_idx]->set_offset(v->get_volume_offset());
            }
            model_object->invalidate_bounding_box();
        }
    }

    // Fixes sinking/flying instances
    for (const std::pair<int, int>& i : done)
    {
        ModelObject* m = m_model->objects[i.first];
        Vec3d shift(0.0, 0.0, -m->get_instance_min_z(i.second));
        m_selection.translate(i.first, i.second, shift);
        m->translate_instance(i.second, shift);
    }

    if (!done.empty())
        post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_ROTATED));

    m_dirty = true;
}

void GLCanvas3D::do_flatten(const Vec3d& normal, const std::string& snapshot_type)
{
    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(_(snapshot_type));

    m_selection.flattening_rotate(normal);
    do_rotate(""); // avoid taking another snapshot
}

void GLCanvas3D::do_mirror(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(_(snapshot_type));

    std::set<std::pair<int, int>> done;  // keeps track of modified instances

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes)
    {
        int object_idx = v->object_idx();
        if ((object_idx < 0) || ((int)m_model->objects.size() <= object_idx))
            continue;

        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        done.insert(std::pair<int, int>(object_idx, instance_idx));

        // Mirror instances/volumes
        ModelObject* model_object = m_model->objects[object_idx];
        if (model_object != nullptr)
        {
            if (selection_mode == Selection::Instance)
                model_object->instances[instance_idx]->set_mirror(v->get_instance_mirror());
            else if (selection_mode == Selection::Volume)
                model_object->volumes[volume_idx]->set_mirror(v->get_volume_mirror());

            model_object->invalidate_bounding_box();
        }
    }

    // Fixes sinking/flying instances
    for (const std::pair<int, int>& i : done)
    {
        ModelObject* m = m_model->objects[i.first];
        Vec3d shift(0.0, 0.0, -m->get_instance_min_z(i.second));
        m_selection.translate(i.first, i.second, shift);
        m->translate_instance(i.second, shift);
    }

    post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));

    m_dirty = true;
}

void GLCanvas3D::update_gizmos_on_off_state()
{
    set_as_dirty();
    m_gizmos.update_data();
    m_gizmos.refresh_on_off_state();
}

void GLCanvas3D::handle_sidebar_focus_event(const std::string& opt_key, bool focus_on)
{
    m_sidebar_field = focus_on ? opt_key : "";

    if (!m_sidebar_field.empty())
        m_gizmos.reset_all_states();

    m_dirty = true;
}

void GLCanvas3D::handle_layers_data_focus_event(const t_layer_height_range range, const EditorType type)
{
    std::string field = "layer_" + std::to_string(type) + "_" + std::to_string(range.first) + "_" + std::to_string(range.second);
    handle_sidebar_focus_event(field, true);
}

void GLCanvas3D::update_ui_from_settings()
{
    m_dirty = true;

#if ENABLE_RETINA_GL
    const float orig_scaling = m_retina_helper->get_scale_factor();

    const bool use_retina = wxGetApp().app_config->get("use_retina_opengl") == "1";
    BOOST_LOG_TRIVIAL(debug) << "GLCanvas3D: Use Retina OpenGL: " << use_retina;
    m_retina_helper->set_use_retina(use_retina);
    const float new_scaling = m_retina_helper->get_scale_factor();

    if (new_scaling != orig_scaling) {
        BOOST_LOG_TRIVIAL(debug) << "GLCanvas3D: Scaling factor: " << new_scaling;

#if ENABLE_NON_STATIC_CANVAS_MANAGER
        Camera& camera = wxGetApp().plater()->get_camera();
        camera.set_zoom(camera.get_zoom() * new_scaling / orig_scaling);
#else
        m_camera.set_zoom(m_camera.get_zoom() * new_scaling / orig_scaling);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
        _refresh_if_shown_on_screen();
    }
#endif
}



GLCanvas3D::WipeTowerInfo GLCanvas3D::get_wipe_tower_info() const
{
    WipeTowerInfo wti;
    
    for (const GLVolume* vol : m_volumes.volumes) {
        if (vol->is_wipe_tower) {
            wti.m_pos = Vec2d(m_config->opt_float("wipe_tower_x"),
                            m_config->opt_float("wipe_tower_y"));
            wti.m_rotation = (M_PI/180.) * m_config->opt_float("wipe_tower_rotation_angle");
            const BoundingBoxf3& bb = vol->bounding_box();
            wti.m_bb_size = Vec2d(bb.size().x(), bb.size().y());
            break;
        }
    }
    
    return wti;
}

Linef3 GLCanvas3D::mouse_ray(const Point& mouse_pos)
{
    float z0 = 0.0f;
    float z1 = 1.0f;
    return Linef3(_mouse_to_3d(mouse_pos, &z0), _mouse_to_3d(mouse_pos, &z1));
}

double GLCanvas3D::get_size_proportional_to_max_bed_size(double factor) const
{
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    return factor * wxGetApp().plater()->get_bed().get_bounding_box(false).max_size();
#else
    return factor * m_bed.get_bounding_box(false).max_size();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
}

void GLCanvas3D::set_cursor(ECursorType type)
{
    if ((m_canvas != nullptr) && (m_cursor_type != type))
    {
        switch (type)
        {
        case Standard: { m_canvas->SetCursor(*wxSTANDARD_CURSOR); break; }
        case Cross: { m_canvas->SetCursor(*wxCROSS_CURSOR); break; }
        }

        m_cursor_type = type;
    }
}

void GLCanvas3D::msw_rescale()
{
    m_warning_texture.msw_rescale(*this);
}

bool GLCanvas3D::has_toolpaths_to_export() const
{
    return m_volumes.has_toolpaths_to_export();
}

void GLCanvas3D::export_toolpaths_to_obj(const char* filename) const
{
    m_volumes.export_toolpaths_to_obj(filename);
}

void GLCanvas3D::mouse_up_cleanup()
{
    m_moving = false;
    m_mouse.drag.move_volume_idx = -1;
    m_mouse.set_start_position_3D_as_invalid();
    m_mouse.set_start_position_2D_as_invalid();
    m_mouse.dragging = false;
    m_mouse.ignore_left_up = false;
    m_dirty = true;

    if (m_canvas->HasCapture())
        m_canvas->ReleaseMouse();
}

bool GLCanvas3D::_is_shown_on_screen() const
{
    return (m_canvas != nullptr) ? m_canvas->IsShownOnScreen() : false;
}

// Getter for the const char*[]
static bool string_getter(const bool is_undo, int idx, const char** out_text)
{
    return wxGetApp().plater()->undo_redo_string_getter(is_undo, idx, out_text);
}

bool GLCanvas3D::_render_undo_redo_stack(const bool is_undo, float pos_x) const
{
    bool action_taken = false;

    ImGuiWrapper* imgui = wxGetApp().imgui();

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    const float x = pos_x * (float)wxGetApp().plater()->get_camera().get_zoom() + 0.5f * (float)get_canvas_size().get_width();
#else
    const float x = pos_x * (float)get_camera().get_zoom() + 0.5f * (float)get_canvas_size().get_width();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    imgui->set_next_window_pos(x, m_undoredo_toolbar.get_height(), ImGuiCond_Always, 0.5f, 0.0f);
    std::string title = is_undo ? L("Undo History") : L("Redo History");
    imgui->begin(_(title), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    int hovered = m_imgui_undo_redo_hovered_pos;
    int selected = -1;
    float em = static_cast<float>(wxGetApp().em_unit());
#if ENABLE_RETINA_GL
	em *= m_retina_helper->get_scale_factor();
#endif

    if (imgui->undo_redo_list(ImVec2(18 * em, 26 * em), is_undo, &string_getter, hovered, selected, m_mouse_wheel))
        m_imgui_undo_redo_hovered_pos = hovered;
    else
        m_imgui_undo_redo_hovered_pos = -1;

    if (selected >= 0)
    {
        is_undo ? wxGetApp().plater()->undo_to(selected) : wxGetApp().plater()->redo_to(selected);
        action_taken = true;
    }

    imgui->text(wxString::Format(is_undo ? _L_PLURAL("Undo %1$d Action", "Undo %1$d Actions", hovered + 1) : _L_PLURAL("Redo %1$d Action", "Redo %1$d Actions", hovered + 1), hovered + 1));

    imgui->end();

    return action_taken;
}

// Getter for the const char*[] for the search list 
static bool search_string_getter(int idx, const char** label, const char** tooltip)
{
    return wxGetApp().plater()->search_string_getter(idx, label, tooltip);
}

bool GLCanvas3D::_render_search_list(float pos_x) const
{
    bool action_taken = false;
    ImGuiWrapper* imgui = wxGetApp().imgui();

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    const float x = pos_x * (float)wxGetApp().plater()->get_camera().get_zoom() + 0.5f * (float)get_canvas_size().get_width();
#else
    const float x = pos_x * (float)get_camera().get_zoom() + 0.5f * (float)get_canvas_size().get_width();
#endif
    imgui->set_next_window_pos(x, m_main_toolbar.get_height(), ImGuiCond_Always, 0.5f, 0.0f);
    std::string title = L("Search");
    imgui->begin(_(title), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    int selected = -1;
    bool edited = false;
    bool check_changed = false;
    float em = static_cast<float>(wxGetApp().em_unit());
#if ENABLE_RETINA_GL
	em *= m_retina_helper->get_scale_factor();
#endif

    Sidebar& sidebar = wxGetApp().sidebar();

    std::string& search_line = sidebar.get_search_line();
    char *s = new char[255];
    strcpy(s, search_line.empty() ? _u8L("Type here to search").c_str() : search_line.c_str());

    imgui->search_list(ImVec2(45 * em, 30 * em), &search_string_getter, s, 
                       sidebar.get_searcher().view_params,
                       selected, edited, m_mouse_wheel);

    search_line = s;
    delete [] s;
    if (search_line == _u8L("Type here to search"))
        search_line.clear();

    if (edited)
        sidebar.search();

    if (selected != size_t(-1)) {
        // selected == 9999 means that Esc kye was pressed
        if (selected != 9999)
            sidebar.jump_to_option(selected);
        action_taken = true;
    }

    imgui->end();

    return action_taken;
}

#define ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT 0
#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
static void debug_output_thumbnail(const ThumbnailData& thumbnail_data)
{
    // debug export of generated image
    wxImage image(thumbnail_data.width, thumbnail_data.height);
    image.InitAlpha();

    for (unsigned int r = 0; r < thumbnail_data.height; ++r)
    {
        unsigned int rr = (thumbnail_data.height - 1 - r) * thumbnail_data.width;
        for (unsigned int c = 0; c < thumbnail_data.width; ++c)
        {
            unsigned char* px = (unsigned char*)thumbnail_data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }

    image.SaveFile("C:/prusa/test/test.png", wxBITMAP_TYPE_PNG);
}
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT

void GLCanvas3D::_render_thumbnail_internal(ThumbnailData& thumbnail_data, bool printable_only, bool parts_only, bool show_bed, bool transparent_background) const
{
    auto is_visible = [](const GLVolume& v) -> bool
    {
        bool ret = v.printable;
        ret &= (!v.shader_outside_printer_detection_enabled || !v.is_outside);
        return ret;
    };

    static const GLfloat orange[] = { 0.923f, 0.504f, 0.264f, 1.0f };
    static const GLfloat gray[] = { 0.64f, 0.64f, 0.64f, 1.0f };

    GLVolumePtrs visible_volumes;

    for (GLVolume* vol : m_volumes.volumes)
    {
        if (!vol->is_modifier && !vol->is_wipe_tower && (!parts_only || (vol->composite_id.volume_id >= 0)))
        {
            if (!printable_only || is_visible(*vol))
                visible_volumes.emplace_back(vol);
        }
    }

    if (visible_volumes.empty())
        return;

    BoundingBoxf3 box;
    for (const GLVolume* vol : visible_volumes)
    {
        box.merge(vol->transformed_bounding_box());
    }

    Camera camera;
    camera.set_type(Camera::Ortho);
    camera.set_scene_box(scene_bounding_box());
    camera.apply_viewport(0, 0, thumbnail_data.width, thumbnail_data.height);
    camera.zoom_to_volumes(visible_volumes);
    camera.apply_view_matrix();

    double near_z = -1.0;
    double far_z = -1.0;

    if (show_bed)
    {
        // extends the near and far z of the frustrum to avoid the bed being clipped

        // box in eye space
#if ENABLE_NON_STATIC_CANVAS_MANAGER
        BoundingBoxf3 t_bed_box = wxGetApp().plater()->get_bed().get_bounding_box(true).transformed(camera.get_view_matrix());
#else
        BoundingBoxf3 t_bed_box = m_bed.get_bounding_box(true).transformed(camera.get_view_matrix());
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
        near_z = -t_bed_box.max(2);
        far_z = -t_bed_box.min(2);
    }

    camera.apply_projection(box, near_z, far_z);

    if (transparent_background)
        glsafe(::glClearColor(0.0f, 0.0f, 0.0f, 0.0f));

    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));

    m_shader.start_using();

    GLint shader_id = m_shader.get_shader_program_id();
    GLint color_id = ::glGetUniformLocation(shader_id, "uniform_color");
    GLint print_box_detection_id = ::glGetUniformLocation(shader_id, "print_box.volume_detection");
    glcheck();

    if (print_box_detection_id != -1)
        glsafe(::glUniform1i(print_box_detection_id, 0));

    for (const GLVolume* vol : visible_volumes)
    {
        if (color_id >= 0)
            glsafe(::glUniform4fv(color_id, 1, (vol->printable && !vol->is_outside) ? orange : gray));
        else
            glsafe(::glColor4fv((vol->printable && !vol->is_outside) ? orange : gray));

        vol->render();
    }

    m_shader.stop_using();

    glsafe(::glDisable(GL_DEPTH_TEST));

    if (show_bed)
        _render_bed(!camera.is_looking_downward(), false);

    if (transparent_background)
        glsafe(::glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
}

void GLCanvas3D::_render_thumbnail_framebuffer(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, bool printable_only, bool parts_only, bool show_bed, bool transparent_background) const
{
    thumbnail_data.set(w, h);
    if (!thumbnail_data.is_valid())
        return;

    bool multisample = m_multisample_allowed;
    if (multisample)
        glsafe(::glEnable(GL_MULTISAMPLE));

    GLint max_samples;
    glsafe(::glGetIntegerv(GL_MAX_SAMPLES, &max_samples));
    GLsizei num_samples = max_samples / 2;

    GLuint render_fbo;
    glsafe(::glGenFramebuffers(1, &render_fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, render_fbo));

    GLuint render_tex = 0;
    GLuint render_tex_buffer = 0;
    if (multisample)
    {
        // use renderbuffer instead of texture to avoid the need to use glTexImage2DMultisample which is available only since OpenGL 3.2
        glsafe(::glGenRenderbuffers(1, &render_tex_buffer));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, render_tex_buffer));
        glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, num_samples, GL_RGBA8, w, h));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, render_tex_buffer));
    }
    else
    {
        glsafe(::glGenTextures(1, &render_tex));
        glsafe(::glBindTexture(GL_TEXTURE_2D, render_tex));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_tex, 0));
    }

    GLuint render_depth;
    glsafe(::glGenRenderbuffers(1, &render_depth));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, render_depth));
    if (multisample)
        glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, num_samples, GL_DEPTH_COMPONENT24, w, h));
    else
        glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, w, h));

    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, render_depth));

    GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, drawBufs));

    if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
    {
        _render_thumbnail_internal(thumbnail_data, printable_only, parts_only, show_bed, transparent_background);

        if (multisample)
        {
            GLuint resolve_fbo;
            glsafe(::glGenFramebuffers(1, &resolve_fbo));
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo));

            GLuint resolve_tex;
            glsafe(::glGenTextures(1, &resolve_tex));
            glsafe(::glBindTexture(GL_TEXTURE_2D, resolve_tex));
            glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
            glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_tex, 0));

            glsafe(::glDrawBuffers(1, drawBufs));

            if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
            {
                glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo));
                glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo));
                glsafe(::glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));

                glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo));
                glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));
            }

            glsafe(::glDeleteTextures(1, &resolve_tex));
            glsafe(::glDeleteFramebuffers(1, &resolve_fbo));
        }
        else
            glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));

#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
        debug_output_thumbnail(thumbnail_data);
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
    }

    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
    glsafe(::glDeleteRenderbuffers(1, &render_depth));
    if (render_tex_buffer != 0)
        glsafe(::glDeleteRenderbuffers(1, &render_tex_buffer));
    if (render_tex != 0)
        glsafe(::glDeleteTextures(1, &render_tex));
    glsafe(::glDeleteFramebuffers(1, &render_fbo));

    if (multisample)
        glsafe(::glDisable(GL_MULTISAMPLE));
}

void GLCanvas3D::_render_thumbnail_framebuffer_ext(ThumbnailData & thumbnail_data, unsigned int w, unsigned int h, bool printable_only, bool parts_only, bool show_bed, bool transparent_background) const
{
    thumbnail_data.set(w, h);
    if (!thumbnail_data.is_valid())
        return;

    bool multisample = m_multisample_allowed;
    if (multisample)
        glsafe(::glEnable(GL_MULTISAMPLE));

    GLint max_samples;
    glsafe(::glGetIntegerv(GL_MAX_SAMPLES_EXT, &max_samples));
    GLsizei num_samples = max_samples / 2;

    GLuint render_fbo;
    glsafe(::glGenFramebuffersEXT(1, &render_fbo));
    glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, render_fbo));

    GLuint render_tex = 0;
    GLuint render_tex_buffer = 0;
    if (multisample)
    {
        // use renderbuffer instead of texture to avoid the need to use glTexImage2DMultisample which is available only since OpenGL 3.2
        glsafe(::glGenRenderbuffersEXT(1, &render_tex_buffer));
        glsafe(::glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, render_tex_buffer));
        glsafe(::glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER_EXT, num_samples, GL_RGBA8, w, h));
        glsafe(::glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, render_tex_buffer));
    }
    else
    {
        glsafe(::glGenTextures(1, &render_tex));
        glsafe(::glBindTexture(GL_TEXTURE_2D, render_tex));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, render_tex, 0));
    }

    GLuint render_depth;
    glsafe(::glGenRenderbuffersEXT(1, &render_depth));
    glsafe(::glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, render_depth));
    if (multisample)
        glsafe(::glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER_EXT, num_samples, GL_DEPTH_COMPONENT24, w, h));
    else
        glsafe(::glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT, w, h));

    glsafe(::glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, render_depth));

    GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, drawBufs));

    if (::glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT)
    {
        _render_thumbnail_internal(thumbnail_data, printable_only, parts_only, show_bed, transparent_background);

        if (multisample)
        {
            GLuint resolve_fbo;
            glsafe(::glGenFramebuffersEXT(1, &resolve_fbo));
            glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, resolve_fbo));

            GLuint resolve_tex;
            glsafe(::glGenTextures(1, &resolve_tex));
            glsafe(::glBindTexture(GL_TEXTURE_2D, resolve_tex));
            glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
            glsafe(::glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, resolve_tex, 0));

            glsafe(::glDrawBuffers(1, drawBufs));

            if (::glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT)
            {
                glsafe(::glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, render_fbo));
                glsafe(::glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, resolve_fbo));
                glsafe(::glBlitFramebufferEXT(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));

                glsafe(::glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, resolve_fbo));
                glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));
            }

            glsafe(::glDeleteTextures(1, &resolve_tex));
            glsafe(::glDeleteFramebuffersEXT(1, &resolve_fbo));
        }
        else
            glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));

#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
        debug_output_thumbnail(thumbnail_data);
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
    }

    glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0));
    glsafe(::glDeleteRenderbuffersEXT(1, &render_depth));
    if (render_tex_buffer != 0)
        glsafe(::glDeleteRenderbuffersEXT(1, &render_tex_buffer));
    if (render_tex != 0)
        glsafe(::glDeleteTextures(1, &render_tex));
    glsafe(::glDeleteFramebuffersEXT(1, &render_fbo));

    if (multisample)
        glsafe(::glDisable(GL_MULTISAMPLE));
}

void GLCanvas3D::_render_thumbnail_legacy(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, bool printable_only, bool parts_only, bool show_bed, bool transparent_background) const
{
    // check that thumbnail size does not exceed the default framebuffer size
    const Size& cnv_size = get_canvas_size();
    unsigned int cnv_w = (unsigned int)cnv_size.get_width();
    unsigned int cnv_h = (unsigned int)cnv_size.get_height();
    if ((w > cnv_w) || (h > cnv_h))
    {
        float ratio = std::min((float)cnv_w / (float)w, (float)cnv_h / (float)h);
        w = (unsigned int)(ratio * (float)w);
        h = (unsigned int)(ratio * (float)h);
    }

    thumbnail_data.set(w, h);
    if (!thumbnail_data.is_valid())
        return;

    _render_thumbnail_internal(thumbnail_data, printable_only, parts_only, show_bed, transparent_background);

    glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));
#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
    debug_output_thumbnail(thumbnail_data);
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT

    // restore the default framebuffer size to avoid flickering on the 3D scene
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    wxGetApp().plater()->get_camera().apply_viewport(0, 0, cnv_size.get_width(), cnv_size.get_height());
#else
    m_camera.apply_viewport(0, 0, cnv_size.get_width(), cnv_size.get_height());
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
}

bool GLCanvas3D::_init_toolbars()
{
    if (!_init_main_toolbar())
        return false;

    if (!_init_undoredo_toolbar())
        return false;

    if (!_init_view_toolbar())
        return false;

    if (!_init_collapse_toolbar())
        return false;

    return true;
}

bool GLCanvas3D::_init_main_toolbar()
{
    if (!m_main_toolbar.is_enabled())
        return true;

    BackgroundTexture::Metadata background_data;
    background_data.filename = "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (!m_main_toolbar.init(background_data))
    {
        // unable to init the toolbar texture, disable it
        m_main_toolbar.set_enabled(false);
        return true;
    }

//    m_main_toolbar.set_layout_type(GLToolbar::Layout::Vertical);
    m_main_toolbar.set_layout_type(GLToolbar::Layout::Horizontal);
    m_main_toolbar.set_horizontal_orientation(GLToolbar::Layout::HO_Right);
    m_main_toolbar.set_vertical_orientation(GLToolbar::Layout::VO_Top);
    m_main_toolbar.set_border(5.0f);
    m_main_toolbar.set_separator_size(5);
    m_main_toolbar.set_gap_size(2);

    GLToolbarItem::Data item;

    item.name = "add";
    item.icon_filename = "add.svg";
    item.tooltip = _utf8(L("Add...")) + " [" + GUI::shortkey_ctrl_prefix() + "I]";
    item.sprite_id = 0;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_ADD)); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "delete";
    item.icon_filename = "remove.svg";
    item.tooltip = _utf8(L("Delete")) + " [Del]";
    item.sprite_id = 1;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_DELETE)); };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_delete(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "deleteall";
    item.icon_filename = "delete_all.svg";
    item.tooltip = _utf8(L("Delete all")) + " [" + GUI::shortkey_ctrl_prefix() + "Del]";
    item.sprite_id = 2;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_DELETE_ALL)); };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_delete_all(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "arrange";
    item.icon_filename = "arrange.svg";
    item.tooltip = _utf8(L("Arrange")) + " [A]\n" + _utf8(L("Arrange selection")) + " [Shift+A]";
    item.sprite_id = 3;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_ARRANGE)); };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_arrange(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    if (!m_main_toolbar.add_separator())
        return false;

    item.name = "copy";
    item.icon_filename = "copy.svg";
    item.tooltip = _utf8(L("Copy")) + " [" + GUI::shortkey_ctrl_prefix() + "C]";
    item.sprite_id = 4;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_COPY)); };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_copy_to_clipboard(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "paste";
    item.icon_filename = "paste.svg";
    item.tooltip = _utf8(L("Paste")) + " [" + GUI::shortkey_ctrl_prefix() + "V]";
    item.sprite_id = 5;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_PASTE)); };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_paste_from_clipboard(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    if (!m_main_toolbar.add_separator())
        return false;

    item.name = "more";
    item.icon_filename = "instance_add.svg";
    item.tooltip = _utf8(L("Add instance")) + " [+]";
    item.sprite_id = 6;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_MORE)); };
    item.visibility_callback = []()->bool { return wxGetApp().get_mode() != comSimple; };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_increase_instances(); };

    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "fewer";
    item.icon_filename = "instance_remove.svg";
    item.tooltip = _utf8(L("Remove instance")) + " [-]";
    item.sprite_id = 7;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_FEWER)); };
    item.visibility_callback = []()->bool { return wxGetApp().get_mode() != comSimple; };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_decrease_instances(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    if (!m_main_toolbar.add_separator())
        return false;

    item.name = "splitobjects";
    item.icon_filename = "split_objects.svg";
    item.tooltip = _utf8(L("Split to objects"));
    item.sprite_id = 8;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_SPLIT_OBJECTS)); };
    item.visibility_callback = GLToolbarItem::Default_Visibility_Callback;
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_split_to_objects(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "splitvolumes";
    item.icon_filename = "split_parts.svg";
    item.tooltip = _utf8(L("Split to parts"));
    item.sprite_id = 9;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_SPLIT_VOLUMES)); };
    item.visibility_callback = []()->bool { return wxGetApp().get_mode() != comSimple; };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_split_to_volumes(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    if (!m_main_toolbar.add_separator())
        return false;

    item.name = "layersediting";
    item.icon_filename = "layers_white.svg";
    item.tooltip = _utf8(L("Variable layer height"));
    item.sprite_id = 10;
    item.left.toggable = true;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_LAYERSEDITING)); };
    item.visibility_callback = [this]()->bool
    {
        bool res = m_process->current_printer_technology() == ptFFF;
        // turns off if changing printer technology
        if (!res && m_main_toolbar.is_item_visible("layersediting") && m_main_toolbar.is_item_pressed("layersediting"))
            force_main_toolbar_left_action(get_main_toolbar_item_id("layersediting"));

        return res;
    };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_layers_editing(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    if (!m_main_toolbar.add_separator())
        return false;

    item.name = "search";
    item.icon_filename = "search_.svg";
    item.tooltip = _utf8(L("Search")) + " [" + GUI::shortkey_ctrl_prefix() + "F]";
    item.sprite_id = 11;
    item.left.render_callback = [this](float left, float right, float, float) {
        if (m_canvas != nullptr)
        {
            if (_render_search_list(0.5f * (left + right)))
                _deactivate_search_toolbar_item();
        }
    };
    item.left.action_callback   = GLToolbarItem::Default_Action_Callback;
    item.visibility_callback    = GLToolbarItem::Default_Visibility_Callback;
    item.enabling_callback      = GLToolbarItem::Default_Enabling_Callback;
    if (!m_main_toolbar.add_item(item))
        return false;

    return true;
}

bool GLCanvas3D::_init_undoredo_toolbar()
{
    if (!m_undoredo_toolbar.is_enabled())
        return true;

    BackgroundTexture::Metadata background_data;
    background_data.filename = "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (!m_undoredo_toolbar.init(background_data))
    {
        // unable to init the toolbar texture, disable it
        m_undoredo_toolbar.set_enabled(false);
        return true;
    }

//    m_undoredo_toolbar.set_layout_type(GLToolbar::Layout::Vertical);
    m_undoredo_toolbar.set_layout_type(GLToolbar::Layout::Horizontal);
    m_undoredo_toolbar.set_horizontal_orientation(GLToolbar::Layout::HO_Left);
    m_undoredo_toolbar.set_vertical_orientation(GLToolbar::Layout::VO_Top);
    m_undoredo_toolbar.set_border(5.0f);
    m_undoredo_toolbar.set_separator_size(5);
    m_undoredo_toolbar.set_gap_size(2);

    GLToolbarItem::Data item;

    item.name = "undo";
    item.icon_filename = "undo_toolbar.svg";
    item.tooltip = _utf8(L("Undo")) + " [" + GUI::shortkey_ctrl_prefix() + "Z]\n" + _utf8(L("Click right mouse button to open/close History"));
    item.sprite_id = 0;
    item.left.action_callback = [this]() { post_event(SimpleEvent(EVT_GLCANVAS_UNDO)); };
    item.right.toggable = true;
    item.right.action_callback = [this]() { m_imgui_undo_redo_hovered_pos = -1; };
    item.right.render_callback = [this](float left, float right, float, float) {
        if (m_canvas != nullptr)
        {
            if (_render_undo_redo_stack(true, 0.5f * (left + right)))
                _deactivate_undo_redo_toolbar_items();
        }
    };
    item.enabling_callback = [this]()->bool {
        bool can_undo = wxGetApp().plater()->can_undo();
        int id = m_undoredo_toolbar.get_item_id("undo");

        std::string curr_additional_tooltip;
        m_undoredo_toolbar.get_additional_tooltip(id, curr_additional_tooltip);

        std::string new_additional_tooltip = "";
        if (can_undo) {
        	std::string action;
            wxGetApp().plater()->undo_redo_topmost_string_getter(true, action);
            new_additional_tooltip = (boost::format(_utf8(L("Next Undo action: %1%"))) % action).str();
        }

        if (new_additional_tooltip != curr_additional_tooltip)
        {
            m_undoredo_toolbar.set_additional_tooltip(id, new_additional_tooltip);
            set_tooltip("");
        }
        return can_undo;
    };

    if (!m_undoredo_toolbar.add_item(item))
        return false;

    item.name = "redo";
    item.icon_filename = "redo_toolbar.svg";
    item.tooltip = _utf8(L("Redo")) + " [" + GUI::shortkey_ctrl_prefix() + "Y]\n" + _utf8(L("Click right mouse button to open/close History"));
    item.sprite_id = 1;
    item.left.action_callback = [this]() { post_event(SimpleEvent(EVT_GLCANVAS_REDO)); };
    item.right.action_callback = [this]() { m_imgui_undo_redo_hovered_pos = -1; };
    item.right.render_callback = [this](float left, float right, float, float) {
        if (m_canvas != nullptr)
        {
            if (_render_undo_redo_stack(false, 0.5f * (left + right)))
                _deactivate_undo_redo_toolbar_items();
        }
    };
    item.enabling_callback = [this]()->bool {
        bool can_redo = wxGetApp().plater()->can_redo();
        int id = m_undoredo_toolbar.get_item_id("redo");

        std::string curr_additional_tooltip;
        m_undoredo_toolbar.get_additional_tooltip(id, curr_additional_tooltip);

        std::string new_additional_tooltip = "";
        if (can_redo) {
        	std::string action;
            wxGetApp().plater()->undo_redo_topmost_string_getter(false, action);
            new_additional_tooltip = (boost::format(_utf8(L("Next Redo action: %1%"))) % action).str();
        }

        if (new_additional_tooltip != curr_additional_tooltip)
        {
            m_undoredo_toolbar.set_additional_tooltip(id, new_additional_tooltip);
            set_tooltip("");
        }
        return can_redo;
    };

    if (!m_undoredo_toolbar.add_item(item))
        return false;

    if (!m_undoredo_toolbar.add_separator())
        return false;

    return true;
}

bool GLCanvas3D::_init_view_toolbar()
{
    return wxGetApp().plater()->init_view_toolbar();
}

bool GLCanvas3D::_init_collapse_toolbar()
{
    if (!m_collapse_toolbar.is_enabled())
        return true;

    BackgroundTexture::Metadata background_data;
    background_data.filename = "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (!m_collapse_toolbar.init(background_data))
    {
        // unable to init the toolbar texture, disable it
        m_collapse_toolbar.set_enabled(false);
        return true;
    }

    m_collapse_toolbar.set_layout_type(GLToolbar::Layout::Vertical);
    m_collapse_toolbar.set_horizontal_orientation(GLToolbar::Layout::HO_Right);
    m_collapse_toolbar.set_vertical_orientation(GLToolbar::Layout::VO_Top);
    m_collapse_toolbar.set_border(5.0f);
    m_collapse_toolbar.set_separator_size(5);
    m_collapse_toolbar.set_gap_size(2);

    GLToolbarItem::Data item;

    item.name = "collapse_sidebar";
    item.icon_filename = "collapse.svg";
    item.tooltip =  wxGetApp().plater()->is_sidebar_collapsed() ? _utf8(L("Expand right panel")) : _utf8(L("Collapse right panel"));
    item.sprite_id = 0;
    item.left.action_callback = [this, item]() {
        std::string new_tooltip = wxGetApp().plater()->is_sidebar_collapsed() ?
            _utf8(L("Collapse right panel")) : _utf8(L("Expand right panel"));

        int id = m_collapse_toolbar.get_item_id("collapse_sidebar");
        m_collapse_toolbar.set_tooltip(id, new_tooltip);
        set_tooltip("");

        wxGetApp().plater()->collapse_sidebar(!wxGetApp().plater()->is_sidebar_collapsed());
    };

    if (!m_collapse_toolbar.add_item(item))
        return false;

    if (!m_collapse_toolbar.add_separator())
        return false;

    item.name = "print";
    item.icon_filename = "cog.svg";
    item.tooltip = _utf8(L("Switch to Print Settings")) + " [" + GUI::shortkey_ctrl_prefix() + "2]";
    item.sprite_id = 1;
    item.left.action_callback = [this]() { wxGetApp().mainframe->select_tab(/*0*/1); };

    if (!m_collapse_toolbar.add_item(item))
        return false;

    item.name = "filament";
    item.icon_filename = "spool.svg";
    item.tooltip = _utf8(L("Switch to Filament Settings")) + " [" + GUI::shortkey_ctrl_prefix() + "3]";
    item.sprite_id = 2;
    item.left.action_callback = [this]() { wxGetApp().mainframe->select_tab(/*1*/2); };
    item.visibility_callback  = [this]() { return wxGetApp().plater()->printer_technology() == ptFFF; };

    if (!m_collapse_toolbar.add_item(item))
        return false;

    item.name = "printer";
    item.icon_filename = "printer.svg";
    item.tooltip = _utf8(L("Switch to Printer Settings")) + " [" + GUI::shortkey_ctrl_prefix() + "4]";
    item.sprite_id = 3;
    item.left.action_callback = [this]() { wxGetApp().mainframe->select_tab(/*2*/3); };

    if (!m_collapse_toolbar.add_item(item))
        return false;

    item.name = "resin";
    item.icon_filename = "resin.svg";
    item.tooltip = _utf8(L("Switch to SLA Material Settings")) + " [" + GUI::shortkey_ctrl_prefix() + "3]";
    item.sprite_id = 4;
    item.left.action_callback = [this]() { wxGetApp().mainframe->select_tab(/*1*/2); };
    item.visibility_callback  = [this]() { return m_process->current_printer_technology() == ptSLA; };

    if (!m_collapse_toolbar.add_item(item))
        return false;

    item.name = "sla_printer";
    item.icon_filename = "sla_printer.svg";
    item.tooltip = _utf8(L("Switch to Printer Settings")) + " [" + GUI::shortkey_ctrl_prefix() + "4]";
    item.sprite_id = 5;
    item.left.action_callback = [this]() { wxGetApp().mainframe->select_tab(/*2*/3); };

    if (!m_collapse_toolbar.add_item(item))
        return false;

    return true;

}

bool GLCanvas3D::_set_current()
{
    return m_context != nullptr && m_canvas->SetCurrent(*m_context);
}

void GLCanvas3D::_resize(unsigned int w, unsigned int h)
{
    if ((m_canvas == nullptr) && (m_context == nullptr))
        return;

    auto *imgui = wxGetApp().imgui();
    imgui->set_display_size((float)w, (float)h);
    const float font_size = 1.5f * wxGetApp().em_unit();
#if ENABLE_RETINA_GL
    imgui->set_scaling(font_size, 1.0f, m_retina_helper->get_scale_factor());
#else
    imgui->set_scaling(font_size, m_canvas->GetContentScaleFactor(), 1.0f);
#endif

    // ensures that this canvas is current
    _set_current();
}

BoundingBoxf3 GLCanvas3D::_max_bounding_box(bool include_gizmos, bool include_bed_model) const
{
    BoundingBoxf3 bb = volumes_bounding_box();

    // The following is a workaround for gizmos not being taken in account when calculating the tight camera frustrum
    // A better solution would ask the gizmo manager for the bounding box of the current active gizmo, if any
    if (include_gizmos && m_gizmos.is_running())
    {
        BoundingBoxf3 sel_bb = m_selection.get_bounding_box();
        Vec3d sel_bb_center = sel_bb.center();
        Vec3d extend_by = sel_bb.max_size() * Vec3d::Ones();
        bb.merge(BoundingBoxf3(sel_bb_center - extend_by, sel_bb_center + extend_by));
    }

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    bb.merge(wxGetApp().plater()->get_bed().get_bounding_box(include_bed_model));
#else
    bb.merge(m_bed.get_bounding_box(include_bed_model));
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    return bb;
}

void GLCanvas3D::_zoom_to_box(const BoundingBoxf3& box, double margin_factor)
{
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    wxGetApp().plater()->get_camera().zoom_to_box(box, margin_factor);
#else
    m_camera.zoom_to_box(box, margin_factor);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    m_dirty = true;
}

void GLCanvas3D::_update_camera_zoom(double zoom)
{
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    wxGetApp().plater()->get_camera().update_zoom(zoom);
#else
    m_camera.update_zoom(zoom);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    m_dirty = true;
}

void GLCanvas3D::_refresh_if_shown_on_screen()
{
    if (_is_shown_on_screen())
    {
        const Size& cnv_size = get_canvas_size();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());

        // Because of performance problems on macOS, where PaintEvents are not delivered
        // frequently enough, we call render() here directly when we can.
        render();
    }
}

void GLCanvas3D::_picking_pass() const
{
    if (m_picking_enabled && !m_mouse.dragging && (m_mouse.position != Vec2d(DBL_MAX, DBL_MAX)))
    {
        m_hover_volume_idxs.clear();

        // Render the object for picking.
        // FIXME This cannot possibly work in a multi - sampled context as the color gets mangled by the anti - aliasing.
        // Better to use software ray - casting on a bounding - box hierarchy.

        if (m_multisample_allowed)
        	// This flag is often ignored by NVIDIA drivers if rendering into a screen buffer.
            glsafe(::glDisable(GL_MULTISAMPLE));

        glsafe(::glDisable(GL_BLEND));
        glsafe(::glEnable(GL_DEPTH_TEST));

        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

        m_camera_clipping_plane = m_gizmos.get_clipping_plane();
        if (m_camera_clipping_plane.is_active()) {
            ::glClipPlane(GL_CLIP_PLANE0, (GLdouble*)m_camera_clipping_plane.get_data());
            ::glEnable(GL_CLIP_PLANE0);
        }
        _render_volumes_for_picking();
        if (m_camera_clipping_plane.is_active())
            ::glDisable(GL_CLIP_PLANE0);

        m_gizmos.render_current_gizmo_for_picking_pass();

        if (m_multisample_allowed)
            glsafe(::glEnable(GL_MULTISAMPLE));

        int volume_id = -1;

        GLubyte color[4] = { 0, 0, 0, 0 };
        const Size& cnv_size = get_canvas_size();
        bool inside = (0 <= m_mouse.position(0)) && (m_mouse.position(0) < cnv_size.get_width()) && (0 <= m_mouse.position(1)) && (m_mouse.position(1) < cnv_size.get_height());
        if (inside)
        {
            glsafe(::glReadPixels(m_mouse.position(0), cnv_size.get_height() - m_mouse.position(1) - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void*)color));
            if (picking_checksum_alpha_channel(color[0], color[1], color[2]) == color[3])
            	// Only non-interpolated colors are valid, those have their lowest three bits zeroed.
            	volume_id = color[0] + (color[1] << 8) + (color[2] << 16);
        }
        if ((0 <= volume_id) && (volume_id < (int)m_volumes.volumes.size()))
        {
            m_hover_volume_idxs.emplace_back(volume_id);
            m_gizmos.set_hover_id(-1);
        }
        else
            m_gizmos.set_hover_id(inside && (unsigned int)volume_id <= GLGizmoBase::BASE_ID ? ((int)GLGizmoBase::BASE_ID - volume_id) : -1);

        _update_volumes_hover_state();
    }
}

void GLCanvas3D::_rectangular_selection_picking_pass() const
{
    m_gizmos.set_hover_id(-1);

    std::set<int> idxs;

    if (m_picking_enabled)
    {
        if (m_multisample_allowed)
        	// This flag is often ignored by NVIDIA drivers if rendering into a screen buffer.
            glsafe(::glDisable(GL_MULTISAMPLE));

        glsafe(::glDisable(GL_BLEND));
        glsafe(::glEnable(GL_DEPTH_TEST));

        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

        _render_volumes_for_picking();

        if (m_multisample_allowed)
            glsafe(::glEnable(GL_MULTISAMPLE));

        int width = std::max((int)m_rectangle_selection.get_width(), 1);
        int height = std::max((int)m_rectangle_selection.get_height(), 1);
        int px_count = width * height;

        int left = (int)m_rectangle_selection.get_left();
        int top = get_canvas_size().get_height() - (int)m_rectangle_selection.get_top();
        if ((left >= 0) && (top >= 0))
        {
#define USE_PARALLEL 1
#if USE_PARALLEL
            struct Pixel
            {
                std::array<GLubyte, 4> data;
            	// Only non-interpolated colors are valid, those have their lowest three bits zeroed.
                bool valid() const { return picking_checksum_alpha_channel(data[0], data[1], data[2]) == data[3]; }
                int id() const { return data[0] + (data[1] << 8) + (data[2] << 16); }
            };

            std::vector<Pixel> frame(px_count);
            glsafe(::glReadPixels(left, top, width, height, GL_RGBA, GL_UNSIGNED_BYTE, (void*)frame.data()));

            tbb::spin_mutex mutex;
            tbb::parallel_for(tbb::blocked_range<size_t>(0, frame.size(), (size_t)width),
                [this, &frame, &idxs, &mutex](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i < range.end(); ++i)
                	if (frame[i].valid()) {
                    	int volume_id = frame[i].id();
                    	if ((0 <= volume_id) && (volume_id < (int)m_volumes.volumes.size())) {
                        	mutex.lock();
                        	idxs.insert(volume_id);
                        	mutex.unlock();
                    	}
                	}
            });
#else
            std::vector<GLubyte> frame(4 * px_count);
            glsafe(::glReadPixels(left, top, width, height, GL_RGBA, GL_UNSIGNED_BYTE, (void*)frame.data()));

            for (int i = 0; i < px_count; ++i)
            {
                int px_id = 4 * i;
                int volume_id = frame[px_id] + (frame[px_id + 1] << 8) + (frame[px_id + 2] << 16);
                if ((0 <= volume_id) && (volume_id < (int)m_volumes.volumes.size()))
                    idxs.insert(volume_id);
            }
#endif // USE_PARALLEL
        }
    }

    m_hover_volume_idxs.assign(idxs.begin(), idxs.end());
    _update_volumes_hover_state();
}

void GLCanvas3D::_render_background() const
{
    glsafe(::glPushMatrix());
    glsafe(::glLoadIdentity());
    glsafe(::glMatrixMode(GL_PROJECTION));
    glsafe(::glPushMatrix());
    glsafe(::glLoadIdentity());

    // Draws a bottom to top gradient over the complete screen.
    glsafe(::glDisable(GL_DEPTH_TEST));

    ::glBegin(GL_QUADS);
    if (m_dynamic_background_enabled && _is_any_volume_outside())
        ::glColor3fv(ERROR_BG_DARK_COLOR);
    else
        ::glColor3fv(DEFAULT_BG_DARK_COLOR);

    ::glVertex2f(-1.0f, -1.0f);
    ::glVertex2f(1.0f, -1.0f);

    if (m_dynamic_background_enabled && _is_any_volume_outside())
        ::glColor3fv(ERROR_BG_LIGHT_COLOR);
    else
        ::glColor3fv(DEFAULT_BG_LIGHT_COLOR);

    ::glVertex2f(1.0f, 1.0f);
    ::glVertex2f(-1.0f, 1.0f);
    glsafe(::glEnd());

    glsafe(::glEnable(GL_DEPTH_TEST));

    glsafe(::glPopMatrix());
    glsafe(::glMatrixMode(GL_MODELVIEW));
    glsafe(::glPopMatrix());
}

void GLCanvas3D::_render_bed(bool bottom, bool show_axes) const
{
    float scale_factor = 1.0;
#if ENABLE_RETINA_GL
    scale_factor = m_retina_helper->get_scale_factor();
#endif // ENABLE_RETINA_GL

    bool show_texture = ! bottom ||
            (m_gizmos.get_current_type() != GLGizmosManager::FdmSupports
          && m_gizmos.get_current_type() != GLGizmosManager::SlaSupports);

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    wxGetApp().plater()->get_bed().render(const_cast<GLCanvas3D&>(*this), bottom, scale_factor, show_axes, show_texture);
#else
    m_bed.render(const_cast<GLCanvas3D&>(*this), theta, scale_factor, show_axes);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
}

void GLCanvas3D::_render_objects() const
{
    if (m_volumes.empty())
        return;

    glsafe(::glEnable(GL_DEPTH_TEST));

    m_camera_clipping_plane = m_gizmos.get_clipping_plane();

    if (m_picking_enabled)
    {
        // Update the layer editing selection to the first object selected, update the current object maximum Z.
        const_cast<LayersEditing&>(m_layers_editing).select_object(*m_model, this->is_layers_editing_enabled() ? m_selection.get_object_idx() : -1);

        if (m_config != nullptr)
        {
#if ENABLE_NON_STATIC_CANVAS_MANAGER
            const BoundingBoxf3& bed_bb = wxGetApp().plater()->get_bed().get_bounding_box(false);
#else
            const BoundingBoxf3& bed_bb = m_bed.get_bounding_box(false);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
            m_volumes.set_print_box((float)bed_bb.min(0), (float)bed_bb.min(1), 0.0f, (float)bed_bb.max(0), (float)bed_bb.max(1), (float)m_config->opt_float("max_print_height"));
            m_volumes.check_outside_state(m_config, nullptr);
        }
    }

    if (m_use_clipping_planes)
        m_volumes.set_z_range(-m_clipping_planes[0].get_data()[3], m_clipping_planes[1].get_data()[3]);
    else
        m_volumes.set_z_range(-FLT_MAX, FLT_MAX);

    m_volumes.set_clipping_plane(m_camera_clipping_plane.get_data());

    m_shader.start_using();
    if (m_picking_enabled && !m_gizmos.is_dragging() && m_layers_editing.is_enabled() && (m_layers_editing.last_object_id != -1) && (m_layers_editing.object_max_z() > 0.0f)) {
        int object_id = m_layers_editing.last_object_id;
#if ENABLE_NON_STATIC_CANVAS_MANAGER
        m_volumes.render(GLVolumeCollection::Opaque, false, wxGetApp().plater()->get_camera().get_view_matrix(), [object_id](const GLVolume& volume) {
            // Which volume to paint without the layer height profile shader?
            return volume.is_active && (volume.is_modifier || volume.composite_id.object_id != object_id);
            });
#else
        m_volumes.render(GLVolumeCollection::Opaque, false, m_camera.get_view_matrix(), [object_id](const GLVolume& volume) {
            // Which volume to paint without the layer height profile shader?
            return volume.is_active && (volume.is_modifier || volume.composite_id.object_id != object_id);
        });
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
        // Let LayersEditing handle rendering of the active object using the layer height profile shader.
        m_layers_editing.render_volumes(*this, this->m_volumes);
    } else {
        // do not cull backfaces to show broken geometry, if any
#if ENABLE_NON_STATIC_CANVAS_MANAGER
        m_volumes.render(GLVolumeCollection::Opaque, m_picking_enabled, wxGetApp().plater()->get_camera().get_view_matrix(), [this](const GLVolume& volume) {
            return (m_render_sla_auxiliaries || volume.composite_id.volume_id >= 0);
            });
#else
        m_volumes.render(GLVolumeCollection::Opaque, m_picking_enabled, m_camera.get_view_matrix(), [this](const GLVolume& volume) {
            return (m_render_sla_auxiliaries || volume.composite_id.volume_id >= 0);
        });
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    }

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    m_volumes.render(GLVolumeCollection::Transparent, false, wxGetApp().plater()->get_camera().get_view_matrix());
#else
    m_volumes.render(GLVolumeCollection::Transparent, false, m_camera.get_view_matrix());
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    m_shader.stop_using();

    m_camera_clipping_plane = ClippingPlane::ClipsNothing();
}

void GLCanvas3D::_render_selection() const
{
    float scale_factor = 1.0;
#if ENABLE_RETINA_GL
    scale_factor = m_retina_helper->get_scale_factor();
#endif

    if (!m_gizmos.is_running())
        m_selection.render(scale_factor);
}

#if ENABLE_RENDER_SELECTION_CENTER
void GLCanvas3D::_render_selection_center() const
{
    m_selection.render_center(m_gizmos.is_dragging());
}
#endif // ENABLE_RENDER_SELECTION_CENTER

void GLCanvas3D::_render_overlays() const
{
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glPushMatrix());
    glsafe(::glLoadIdentity());
    // ensure that the textures are renderered inside the frustrum
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    const Camera& camera = wxGetApp().plater()->get_camera();
    glsafe(::glTranslated(0.0, 0.0, -(camera.get_near_z() + 0.005)));
    // ensure that the overlay fits the frustrum near z plane
    double gui_scale = camera.get_gui_scale();
#else
    glsafe(::glTranslated(0.0, 0.0, -(m_camera.get_near_z() + 0.005)));
    // ensure that the overlay fits the frustrum near z plane
    double gui_scale = m_camera.get_gui_scale();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    glsafe(::glScaled(gui_scale, gui_scale, 1.0));

    _render_gizmos_overlay();
    _render_warning_texture();
    _render_legend_texture();

    // main toolbar and undoredo toolbar need to be both updated before rendering because both their sizes are needed
    // to correctly place them
#if ENABLE_RETINA_GL
    const float scale = m_retina_helper->get_scale_factor() * wxGetApp().toolbar_icon_scale(true);
    m_main_toolbar.set_scale(scale);
    m_undoredo_toolbar.set_scale(scale);
    m_collapse_toolbar.set_scale(scale);
#else
    const float size = int(GLToolbar::Default_Icons_Size * wxGetApp().toolbar_icon_scale(true));
    m_main_toolbar.set_icons_size(size);
    m_undoredo_toolbar.set_icons_size(size);
    m_collapse_toolbar.set_icons_size(size);
#endif // ENABLE_RETINA_GL

    _render_main_toolbar();
    _render_undoredo_toolbar();
    _render_collapse_toolbar();
    _render_view_toolbar();

    if ((m_layers_editing.last_object_id >= 0) && (m_layers_editing.object_max_z() > 0.0f))
        m_layers_editing.render_overlay(*this);

    const ConfigOptionBool* opt = dynamic_cast<const ConfigOptionBool*>(m_config->option("complete_objects"));
    bool sequential_print = opt != nullptr && opt->value;
    std::vector<const ModelInstance*> sorted_instances;
    if (sequential_print) {
        for (ModelObject* model_object : m_model->objects)
            for (ModelInstance* model_instance : model_object->instances) {
                sorted_instances.emplace_back(model_instance);
            }
    }
    m_labels.render(sorted_instances);

#if ENABLE_SLOPE_RENDERING
    m_slope.render();
#endif // ENABLE_SLOPE_RENDERING

    glsafe(::glPopMatrix());
}

void GLCanvas3D::_render_warning_texture() const
{
    m_warning_texture.render(*this);
}

void GLCanvas3D::_render_legend_texture() const
{
    if (!m_legend_texture_enabled)
        return;

    m_legend_texture.render(*this);
}

void GLCanvas3D::_render_volumes_for_picking() const
{
    static const GLfloat INV_255 = 1.0f / 255.0f;

    // do not cull backfaces to show broken geometry, if any
    glsafe(::glDisable(GL_CULL_FACE));

    glsafe(::glEnableClientState(GL_VERTEX_ARRAY));
    glsafe(::glEnableClientState(GL_NORMAL_ARRAY));

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    const Transform3d& view_matrix = wxGetApp().plater()->get_camera().get_view_matrix();
#else
    const Transform3d& view_matrix = m_camera.get_view_matrix();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    for (size_t type = 0; type < 2; ++ type) {
	    GLVolumeWithIdAndZList to_render = volumes_to_render(m_volumes.volumes, (type == 0) ? GLVolumeCollection::Opaque : GLVolumeCollection::Transparent, view_matrix);
	    for (const GLVolumeWithIdAndZ& volume : to_render)
	        if (!volume.first->disabled && ((volume.first->composite_id.volume_id >= 0) || m_render_sla_auxiliaries)) {
		        // Object picking mode. Render the object with a color encoding the object index.
		        unsigned int id = volume.second.first;
		        unsigned int r = (id & (0x000000FF << 0)) << 0;
		        unsigned int g = (id & (0x000000FF << 8)) >> 8;
		        unsigned int b = (id & (0x000000FF << 16)) >> 16;
		        unsigned int a = picking_checksum_alpha_channel(r, g, b);
		        glsafe(::glColor4f((GLfloat)r * INV_255, (GLfloat)g * INV_255, (GLfloat)b * INV_255, (GLfloat)a * INV_255));
	            volume.first->render();
	        }
	}

    glsafe(::glDisableClientState(GL_NORMAL_ARRAY));
    glsafe(::glDisableClientState(GL_VERTEX_ARRAY));

    glsafe(::glEnable(GL_CULL_FACE));
}

void GLCanvas3D::_render_current_gizmo() const
{
    m_gizmos.render_current_gizmo();
}

void GLCanvas3D::_render_gizmos_overlay() const
{
#if ENABLE_RETINA_GL
//     m_gizmos.set_overlay_scale(m_retina_helper->get_scale_factor());
    const float scale = m_retina_helper->get_scale_factor()*wxGetApp().toolbar_icon_scale();
    m_gizmos.set_overlay_scale(scale); //! #ys_FIXME_experiment
#else
//     m_gizmos.set_overlay_scale(m_canvas->GetContentScaleFactor());
//     m_gizmos.set_overlay_scale(wxGetApp().em_unit()*0.1f);
    const float size = int(GLGizmosManager::Default_Icons_Size*wxGetApp().toolbar_icon_scale());
    m_gizmos.set_overlay_icon_size(size); //! #ys_FIXME_experiment
#endif /* __WXMSW__ */

    m_gizmos.render_overlay();
}

void GLCanvas3D::_render_main_toolbar() const
{
    if (!m_main_toolbar.is_enabled())
        return;

    Size cnv_size = get_canvas_size();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    float inv_zoom = (float)wxGetApp().plater()->get_camera().get_inv_zoom();
#else
    float inv_zoom = (float)m_camera.get_inv_zoom();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

    float top = 0.5f * (float)cnv_size.get_height() * inv_zoom;
    float left = -0.5f * (m_main_toolbar.get_width() + m_undoredo_toolbar.get_width()) * inv_zoom;

    m_main_toolbar.set_position(top, left);
    m_main_toolbar.render(*this);
}

void GLCanvas3D::_render_undoredo_toolbar() const
{
    if (!m_undoredo_toolbar.is_enabled())
        return;

    Size cnv_size = get_canvas_size();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    float inv_zoom = (float)wxGetApp().plater()->get_camera().get_inv_zoom();
#else
    float inv_zoom = (float)m_camera.get_inv_zoom();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

    float top = 0.5f * (float)cnv_size.get_height() * inv_zoom;
    float left = (m_main_toolbar.get_width() - 0.5f * (m_main_toolbar.get_width() + m_undoredo_toolbar.get_width())) * inv_zoom;
    m_undoredo_toolbar.set_position(top, left);
    m_undoredo_toolbar.render(*this);
}

void GLCanvas3D::_render_collapse_toolbar() const
{
    if (!m_collapse_toolbar.is_enabled())
        return;

    Size cnv_size = get_canvas_size();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    float inv_zoom = (float)wxGetApp().plater()->get_camera().get_inv_zoom();
#else
    float inv_zoom = (float)m_camera.get_inv_zoom();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

    float band = m_layers_editing.is_enabled() ? (wxGetApp().imgui()->get_style_scaling() * LayersEditing::THICKNESS_BAR_WIDTH) : 0.0;

    float top  = 0.5f * (float)cnv_size.get_height() * inv_zoom;
    float left = (0.5f * (float)cnv_size.get_width() - (float)m_collapse_toolbar.get_width() - band) * inv_zoom;

    m_collapse_toolbar.set_position(top, left);
    m_collapse_toolbar.render(*this);
}

void GLCanvas3D::_render_view_toolbar() const
{
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    GLToolbar& view_toolbar = wxGetApp().plater()->get_view_toolbar();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

#if ENABLE_RETINA_GL
//     m_view_toolbar.set_scale(m_retina_helper->get_scale_factor());
    const float scale = m_retina_helper->get_scale_factor() * wxGetApp().toolbar_icon_scale();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    view_toolbar.set_scale(scale); //! #ys_FIXME_experiment
#else
    m_view_toolbar.set_scale(scale); //! #ys_FIXME_experiment
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
#else
//     m_view_toolbar.set_scale(m_canvas->GetContentScaleFactor());
//     m_view_toolbar.set_scale(wxGetApp().em_unit()*0.1f);
    const float size = int(GLGizmosManager::Default_Icons_Size * wxGetApp().toolbar_icon_scale());
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    view_toolbar.set_icons_size(size); //! #ys_FIXME_experiment
#else
    m_view_toolbar.set_icons_size(size); //! #ys_FIXME_experiment
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
#endif // ENABLE_RETINA_GL

    Size cnv_size = get_canvas_size();
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    float inv_zoom = (float)wxGetApp().plater()->get_camera().get_inv_zoom();
#else
    float inv_zoom = (float)m_camera.get_inv_zoom();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

    // places the toolbar on the bottom-left corner of the 3d scene
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    float top = (-0.5f * (float)cnv_size.get_height() + view_toolbar.get_height()) * inv_zoom;
#else
    float top = (-0.5f * (float)cnv_size.get_height() + m_view_toolbar.get_height()) * inv_zoom;
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
    float left = -0.5f * (float)cnv_size.get_width() * inv_zoom;
#if ENABLE_NON_STATIC_CANVAS_MANAGER
    view_toolbar.set_position(top, left);
    view_toolbar.render(*this);
#else
    m_view_toolbar.set_position(top, left);
    m_view_toolbar.render(*this);
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER
}

#if ENABLE_SHOW_CAMERA_TARGET
void GLCanvas3D::_render_camera_target() const
{
    double half_length = 5.0;

    glsafe(::glDisable(GL_DEPTH_TEST));

    glsafe(::glLineWidth(2.0f));
    ::glBegin(GL_LINES);
    const Vec3d& target = m_camera.get_target();
    // draw line for x axis
    ::glColor3f(1.0f, 0.0f, 0.0f);
    ::glVertex3d(target(0) - half_length, target(1), target(2));
    ::glVertex3d(target(0) + half_length, target(1), target(2));
    // draw line for y axis
    ::glColor3f(0.0f, 1.0f, 0.0f);
    ::glVertex3d(target(0), target(1) - half_length, target(2));
    ::glVertex3d(target(0), target(1) + half_length, target(2));
    // draw line for z axis
    ::glColor3f(0.0f, 0.0f, 1.0f);
    ::glVertex3d(target(0), target(1), target(2) - half_length);
    ::glVertex3d(target(0), target(1), target(2) + half_length);
    glsafe(::glEnd());
}
#endif // ENABLE_SHOW_CAMERA_TARGET

void GLCanvas3D::_render_sla_slices() const
{
    if (!m_use_clipping_planes || wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptSLA)
        return;

    const SLAPrint* print = this->sla_print();
    const PrintObjects& print_objects = print->objects();
    if (print_objects.empty())
        // nothing to render, return
        return;

    double clip_min_z = -m_clipping_planes[0].get_data()[3];
    double clip_max_z = m_clipping_planes[1].get_data()[3];
    for (unsigned int i = 0; i < (unsigned int)print_objects.size(); ++i)
    {
        const SLAPrintObject* obj = print_objects[i];

        if (!obj->is_step_done(slaposSliceSupports))
            continue;

        SlaCap::ObjectIdToTrianglesMap::iterator it_caps_bottom = m_sla_caps[0].triangles.find(i);
        SlaCap::ObjectIdToTrianglesMap::iterator it_caps_top    = m_sla_caps[1].triangles.find(i);
        {
			if (it_caps_bottom == m_sla_caps[0].triangles.end())
				it_caps_bottom = m_sla_caps[0].triangles.emplace(i, SlaCap::Triangles()).first;
            if (! m_sla_caps[0].matches(clip_min_z)) {
				m_sla_caps[0].z = clip_min_z;
                it_caps_bottom->second.object.clear();
                it_caps_bottom->second.supports.clear();
            }
            if (it_caps_top == m_sla_caps[1].triangles.end())
				it_caps_top = m_sla_caps[1].triangles.emplace(i, SlaCap::Triangles()).first;
            if (! m_sla_caps[1].matches(clip_max_z)) {
				m_sla_caps[1].z = clip_max_z;
                it_caps_top->second.object.clear();
                it_caps_top->second.supports.clear();
            }
        }
        Pointf3s &bottom_obj_triangles = it_caps_bottom->second.object;
        Pointf3s &bottom_sup_triangles = it_caps_bottom->second.supports;
        Pointf3s &top_obj_triangles    = it_caps_top->second.object;
        Pointf3s &top_sup_triangles    = it_caps_top->second.supports;

        if ((bottom_obj_triangles.empty() || bottom_sup_triangles.empty() || top_obj_triangles.empty() || top_sup_triangles.empty()) &&
            !obj->get_slice_index().empty())
        {
            double layer_height         = print->default_object_config().layer_height.value;
            double initial_layer_height = print->material_config().initial_layer_height.value;
            bool   left_handed          = obj->is_left_handed();

            coord_t key_zero = obj->get_slice_index().front().print_level();
            // Slice at the center of the slab starting at clip_min_z will be rendered for the lower plane.
            coord_t key_low  = coord_t((clip_min_z - initial_layer_height + layer_height) / SCALING_FACTOR) + key_zero;
            // Slice at the center of the slab ending at clip_max_z will be rendered for the upper plane.
            coord_t key_high = coord_t((clip_max_z - initial_layer_height) / SCALING_FACTOR) + key_zero;

            const SliceRecord& slice_low  = obj->closest_slice_to_print_level(key_low, coord_t(SCALED_EPSILON));
            const SliceRecord& slice_high = obj->closest_slice_to_print_level(key_high, coord_t(SCALED_EPSILON));

            // Offset to avoid OpenGL Z fighting between the object's horizontal surfaces and the triangluated surfaces of the cuts.
            double plane_shift_z = 0.002;

            if (slice_low.is_valid()) {
                const ExPolygons& obj_bottom = slice_low.get_slice(soModel);
                const ExPolygons& sup_bottom = slice_low.get_slice(soSupport);
                // calculate model bottom cap
                if (bottom_obj_triangles.empty() && !obj_bottom.empty())
                    bottom_obj_triangles = triangulate_expolygons_3d(obj_bottom, clip_min_z - plane_shift_z, ! left_handed);
                // calculate support bottom cap
                if (bottom_sup_triangles.empty() && !sup_bottom.empty())
                    bottom_sup_triangles = triangulate_expolygons_3d(sup_bottom, clip_min_z - plane_shift_z, ! left_handed);
            }

            if (slice_high.is_valid()) {
                const ExPolygons& obj_top = slice_high.get_slice(soModel);
                const ExPolygons& sup_top = slice_high.get_slice(soSupport);
                // calculate model top cap
                if (top_obj_triangles.empty() && !obj_top.empty())
                    top_obj_triangles = triangulate_expolygons_3d(obj_top, clip_max_z + plane_shift_z, left_handed);
                // calculate support top cap
                if (top_sup_triangles.empty() && !sup_top.empty())
                    top_sup_triangles = triangulate_expolygons_3d(sup_top, clip_max_z + plane_shift_z, left_handed);
            }
        }

        if (!bottom_obj_triangles.empty() || !top_obj_triangles.empty() || !bottom_sup_triangles.empty() || !top_sup_triangles.empty())
        {
			for (const SLAPrintObject::Instance& inst : obj->instances())
            {
                glsafe(::glPushMatrix());
                glsafe(::glTranslated(unscale<double>(inst.shift.x()), unscale<double>(inst.shift.y()), 0));
                glsafe(::glRotatef(Geometry::rad2deg(inst.rotation), 0.0, 0.0, 1.0));
				if (obj->is_left_handed())
                    // The polygons are mirrored by X.
                    glsafe(::glScalef(-1.0, 1.0, 1.0));
                glsafe(::glEnableClientState(GL_VERTEX_ARRAY));
                glsafe(::glColor3f(1.0f, 0.37f, 0.0f));
				if (!bottom_obj_triangles.empty()) {
                    glsafe(::glVertexPointer(3, GL_DOUBLE, 0, (GLdouble*)bottom_obj_triangles.front().data()));
                    glsafe(::glDrawArrays(GL_TRIANGLES, 0, bottom_obj_triangles.size()));
				}
				if (! top_obj_triangles.empty()) {
                    glsafe(::glVertexPointer(3, GL_DOUBLE, 0, (GLdouble*)top_obj_triangles.front().data()));
                    glsafe(::glDrawArrays(GL_TRIANGLES, 0, top_obj_triangles.size()));
				}
                glsafe(::glColor3f(1.0f, 0.0f, 0.37f));
				if (! bottom_sup_triangles.empty()) {
                    glsafe(::glVertexPointer(3, GL_DOUBLE, 0, (GLdouble*)bottom_sup_triangles.front().data()));
                    glsafe(::glDrawArrays(GL_TRIANGLES, 0, bottom_sup_triangles.size()));
				}
				if (! top_sup_triangles.empty()) {
                    glsafe(::glVertexPointer(3, GL_DOUBLE, 0, (GLdouble*)top_sup_triangles.front().data()));
                    glsafe(::glDrawArrays(GL_TRIANGLES, 0, top_sup_triangles.size()));
				}
                glsafe(::glDisableClientState(GL_VERTEX_ARRAY));
                glsafe(::glPopMatrix());
            }
        }
    }
}

void GLCanvas3D::_render_selection_sidebar_hints() const
{
    m_selection.render_sidebar_hints(m_sidebar_field, m_shader);
}

void GLCanvas3D::_update_volumes_hover_state() const
{
    for (GLVolume* v : m_volumes.volumes)
    {
        v->hover = GLVolume::HS_None;
    }

    if (m_hover_volume_idxs.empty())
        return;

    bool ctrl_pressed = wxGetKeyState(WXK_CONTROL); // additive select/deselect
    bool shift_pressed = wxGetKeyState(WXK_SHIFT);  // select by rectangle
    bool alt_pressed = wxGetKeyState(WXK_ALT);      // deselect by rectangle

    if (alt_pressed && (shift_pressed || ctrl_pressed))
    {
        // illegal combinations of keys
        m_hover_volume_idxs.clear();
        return;
    }

    bool selection_modifiers_only = m_selection.is_empty() || m_selection.is_any_modifier();

    bool hover_modifiers_only = true;
    for (int i : m_hover_volume_idxs)
    {
        if (!m_volumes.volumes[i]->is_modifier)
        {
            hover_modifiers_only = false;
            break;
        }
    }

    std::set<std::pair<int, int>> hover_instances;
    for (int i : m_hover_volume_idxs)
    {
        const GLVolume& v = *m_volumes.volumes[i];
        hover_instances.insert(std::make_pair(v.object_idx(), v.instance_idx()));
    }

    bool hover_from_single_instance = hover_instances.size() == 1;

    if (hover_modifiers_only && !hover_from_single_instance)
    {
        // do not allow to select volumes from different instances
        m_hover_volume_idxs.clear();
        return;
    }

    for (int i : m_hover_volume_idxs)
    {
        GLVolume& volume = *m_volumes.volumes[i];
        if (volume.hover != GLVolume::HS_None)
            continue;

        bool deselect = volume.selected && ((ctrl_pressed && !shift_pressed) || alt_pressed);
        // (volume->is_modifier && !selection_modifiers_only && !is_ctrl_pressed) -> allows hovering on selected modifiers belonging to selection of type Instance
        bool select = (!volume.selected || (volume.is_modifier && !selection_modifiers_only && !ctrl_pressed)) && !alt_pressed;

        if (select || deselect)
        {
            bool as_volume =
                volume.is_modifier && hover_from_single_instance && !ctrl_pressed &&
                (
                (!deselect) ||
                (deselect && !m_selection.is_single_full_instance() && (volume.object_idx() == m_selection.get_object_idx()) && (volume.instance_idx() == m_selection.get_instance_idx()))
                );

            if (as_volume)
            {
                if (deselect)
                    volume.hover = GLVolume::HS_Deselect;
                else
                    volume.hover = GLVolume::HS_Select;
            }
            else
            {
                int object_idx = volume.object_idx();
                int instance_idx = volume.instance_idx();

                for (GLVolume* v : m_volumes.volumes)
                {
                    if ((v->object_idx() == object_idx) && (v->instance_idx() == instance_idx))
                    {
                        if (deselect)
                            v->hover = GLVolume::HS_Deselect;
                        else
                            v->hover = GLVolume::HS_Select;
                    }
                }
            }
        }
    }
}

void GLCanvas3D::_perform_layer_editing_action(wxMouseEvent* evt)
{
    int object_idx_selected = m_layers_editing.last_object_id;
    if (object_idx_selected == -1)
        return;

    // A volume is selected. Test, whether hovering over a layer thickness bar.
    if (evt != nullptr)
    {
        const Rect& rect = LayersEditing::get_bar_rect_screen(*this);
        float b = rect.get_bottom();
        m_layers_editing.last_z = m_layers_editing.object_max_z() * (b - evt->GetY() - 1.0f) / (b - rect.get_top());
        m_layers_editing.last_action = 
            evt->ShiftDown() ? (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_SMOOTH : LAYER_HEIGHT_EDIT_ACTION_REDUCE) : 
                               (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_INCREASE : LAYER_HEIGHT_EDIT_ACTION_DECREASE);
    }

    m_layers_editing.adjust_layer_height_profile();
    _refresh_if_shown_on_screen();

    // Automatic action on mouse down with the same coordinate.
    _start_timer();
}

Vec3d GLCanvas3D::_mouse_to_3d(const Point& mouse_pos, float* z)
{
    if (m_canvas == nullptr)
        return Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);

#if ENABLE_NON_STATIC_CANVAS_MANAGER
    const Camera& camera = wxGetApp().plater()->get_camera();
    const std::array<int, 4>& viewport = camera.get_viewport();
    const Transform3d& modelview_matrix = camera.get_view_matrix();
    const Transform3d& projection_matrix = camera.get_projection_matrix();
#else
    const std::array<int, 4>& viewport = m_camera.get_viewport();
    const Transform3d& modelview_matrix = m_camera.get_view_matrix();
    const Transform3d& projection_matrix = m_camera.get_projection_matrix();
#endif // ENABLE_NON_STATIC_CANVAS_MANAGER

    GLint y = viewport[3] - (GLint)mouse_pos(1);
    GLfloat mouse_z;
    if (z == nullptr)
        glsafe(::glReadPixels((GLint)mouse_pos(0), y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, (void*)&mouse_z));
    else
        mouse_z = *z;

    GLdouble out_x, out_y, out_z;
    ::gluUnProject((GLdouble)mouse_pos(0), (GLdouble)y, (GLdouble)mouse_z, (GLdouble*)modelview_matrix.data(), (GLdouble*)projection_matrix.data(), (GLint*)viewport.data(), &out_x, &out_y, &out_z);
    return Vec3d((double)out_x, (double)out_y, (double)out_z);
}

Vec3d GLCanvas3D::_mouse_to_bed_3d(const Point& mouse_pos)
{
    return mouse_ray(mouse_pos).intersect_plane(0.0);
}

void GLCanvas3D::_start_timer()
{
    m_timer.Start(100, wxTIMER_CONTINUOUS);
}

void GLCanvas3D::_stop_timer()
{
    m_timer.Stop();
}

void GLCanvas3D::_load_print_toolpaths()
{
    const Print *print = this->fff_print();
    if (print == nullptr)
        return;

    if (!print->is_step_done(psSkirt) || !print->is_step_done(psBrim))
        return;

    if (!print->has_skirt() && (print->config().brim_width.value == 0))
        return;

    const float color[] = { 0.5f, 1.0f, 0.5f, 1.0f }; // greenish

    // number of skirt layers
    size_t total_layer_count = 0;
    for (const PrintObject* print_object : print->objects())
    {
        total_layer_count = std::max(total_layer_count, print_object->total_layer_count());
    }
    size_t skirt_height = print->has_infinite_skirt() ? total_layer_count : std::min<size_t>(print->config().skirt_height.value, total_layer_count);
    if ((skirt_height == 0) && (print->config().brim_width.value > 0))
        skirt_height = 1;

    // Get first skirt_height layers.
    //FIXME This code is fishy. It may not work for multiple objects with different layering due to variable layer height feature.
    // This is not critical as this is just an initial preview.
    const PrintObject* highest_object = *std::max_element(print->objects().begin(), print->objects().end(), [](auto l, auto r){ return l->layers().size() < r->layers().size(); });
    std::vector<float> print_zs;
    print_zs.reserve(skirt_height * 2);
    for (size_t i = 0; i < std::min(skirt_height, highest_object->layers().size()); ++ i)
        print_zs.emplace_back(float(highest_object->layers()[i]->print_z));
    // Only add skirt for the raft layers.
    for (size_t i = 0; i < std::min(skirt_height, std::min(highest_object->slicing_parameters().raft_layers(), highest_object->support_layers().size())); ++ i)
        print_zs.emplace_back(float(highest_object->support_layers()[i]->print_z));
    sort_remove_duplicates(print_zs);
    skirt_height = std::min(skirt_height, print_zs.size());
    print_zs.erase(print_zs.begin() + skirt_height, print_zs.end());

    GLVolume *volume = m_volumes.new_toolpath_volume(color, VERTEX_BUFFER_RESERVE_SIZE);
    for (size_t i = 0; i < skirt_height; ++ i) {
        volume->print_zs.emplace_back(print_zs[i]);
        volume->offsets.emplace_back(volume->indexed_vertex_array.quad_indices.size());
        volume->offsets.emplace_back(volume->indexed_vertex_array.triangle_indices.size());
        if (i == 0)
            _3DScene::extrusionentity_to_verts(print->brim(), print_zs[i], Point(0, 0), *volume);
        _3DScene::extrusionentity_to_verts(print->skirt(), print_zs[i], Point(0, 0), *volume);
        // Ensure that no volume grows over the limits. If the volume is too large, allocate a new one.
        if (volume->indexed_vertex_array.vertices_and_normals_interleaved.size() > MAX_VERTEX_BUFFER_SIZE) {
        	GLVolume &vol = *volume;
            volume = m_volumes.new_toolpath_volume(vol.color);
            reserve_new_volume_finalize_old_volume(*volume, vol, m_initialized);
        }
    }
    volume->indexed_vertex_array.finalize_geometry(m_initialized);
}

void GLCanvas3D::_load_print_object_toolpaths(const PrintObject& print_object, const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values)
{
    std::vector<float> tool_colors = _parse_colors(str_tool_colors);

    struct Ctxt
    {
        const PrintInstances        *shifted_copies;
        std::vector<const Layer*>    layers;
        bool                         has_perimeters;
        bool                         has_infill;
        bool                         has_support;
        const std::vector<float>*    tool_colors;
        bool                         is_single_material_print;
        int                          extruders_cnt;
        const std::vector<CustomGCode::Item>*   color_print_values;

        static const float*          color_perimeters() { static float color[4] = { 1.0f, 1.0f, 0.0f, 1.f }; return color; } // yellow
        static const float*          color_infill() { static float color[4] = { 1.0f, 0.5f, 0.5f, 1.f }; return color; } // redish
        static const float*          color_support() { static float color[4] = { 0.5f, 1.0f, 0.5f, 1.f }; return color; } // greenish
        static const float*          color_pause_or_custom_code() { static float color[4] = { 0.5f, 0.5f, 0.5f, 1.f }; return color; } // gray

        // For cloring by a tool, return a parsed color.
        bool                         color_by_tool() const { return tool_colors != nullptr; }
        size_t                       number_tools()  const { return this->color_by_tool() ? tool_colors->size() / 4 : 0; }
        const float*                 color_tool(size_t tool) const { return tool_colors->data() + tool * 4; }

        // For coloring by a color_print(M600), return a parsed color.
        bool                         color_by_color_print() const { return color_print_values!=nullptr; }
        const size_t                 color_print_color_idx_by_layer_idx(const size_t layer_idx) const {
            const CustomGCode::Item value{layers[layer_idx]->print_z + EPSILON, "", 0, ""};
            auto it = std::lower_bound(color_print_values->begin(), color_print_values->end(), value);
            return (it - color_print_values->begin()) % number_tools();
        }

        const size_t                 color_print_color_idx_by_layer_idx_and_extruder(const size_t layer_idx, const int extruder) const
        {
            const coordf_t print_z = layers[layer_idx]->print_z;

            auto it = std::find_if(color_print_values->begin(), color_print_values->end(),
                [print_z](const CustomGCode::Item& code)
                { return fabs(code.print_z - print_z) < EPSILON; });
            if (it != color_print_values->end())
            {
                const std::string& code = it->gcode;
                // pause print or custom Gcode
                if (code == PausePrintCode || 
                    (code != ColorChangeCode && code != ToolChangeCode))
                    return number_tools()-1; // last color item is a gray color for pause print or custom G-code 

                // change tool (extruder) 
                if (code == ToolChangeCode)
                    return get_color_idx_for_tool_change(it, extruder);
                // change color for current extruder
                if (code == ColorChangeCode) {
                    int color_idx = get_color_idx_for_color_change(it, extruder);
                    if (color_idx >= 0)
                        return color_idx;
                }
            }

            const CustomGCode::Item value{print_z + EPSILON, "", 0, ""};
            it = std::lower_bound(color_print_values->begin(), color_print_values->end(), value);
            while (it != color_print_values->begin())
            {
                --it;
                // change color for current extruder
                if (it->gcode == ColorChangeCode) {
                    int color_idx = get_color_idx_for_color_change(it, extruder);
                    if (color_idx >= 0)
                        return color_idx;
                }
                // change tool (extruder) 
                if (it->gcode == ToolChangeCode)
                    return get_color_idx_for_tool_change(it, extruder);
            }

            return std::min<int>(extruders_cnt - 1, std::max<int>(extruder - 1, 0));;
        }

    private:
        int get_m600_color_idx(std::vector<CustomGCode::Item>::const_iterator it) const
        {
            int shift = 0;
            while (it != color_print_values->begin()) {
                --it;
                if (it->gcode == ColorChangeCode)
                    shift++;
            }
            return extruders_cnt + shift;
        }

        int get_color_idx_for_tool_change(std::vector<CustomGCode::Item>::const_iterator it, const int extruder) const
        {
            const int current_extruder = it->extruder == 0 ? extruder : it->extruder;
            if (number_tools() == extruders_cnt + 1) // there is no one "M600"
                return std::min<int>(extruders_cnt - 1, std::max<int>(current_extruder - 1, 0));

            auto it_n = it;
            while (it_n != color_print_values->begin()) {
                --it_n;
                if (it_n->gcode == ColorChangeCode && it_n->extruder == current_extruder)
                    return get_m600_color_idx(it_n);
            }

            return std::min<int>(extruders_cnt - 1, std::max<int>(current_extruder - 1, 0));
        }

        int get_color_idx_for_color_change(std::vector<CustomGCode::Item>::const_iterator it, const int extruder) const
        {
            if (extruders_cnt == 1)
                return get_m600_color_idx(it);

            auto it_n = it;
            bool is_tool_change = false;
            while (it_n != color_print_values->begin()) {
                --it_n;
                if (it_n->gcode == ToolChangeCode) {
                    is_tool_change = true;
                    if (it_n->extruder == it->extruder || (it_n->extruder == 0 && it->extruder == extruder))
                        return get_m600_color_idx(it);
                    break;
                }
            }
            if (!is_tool_change && it->extruder == extruder)
                return get_m600_color_idx(it);

            return -1;
        }

    } ctxt;

    ctxt.has_perimeters = print_object.is_step_done(posPerimeters);
    ctxt.has_infill = print_object.is_step_done(posInfill);
    ctxt.has_support = print_object.is_step_done(posSupportMaterial);
    ctxt.tool_colors = tool_colors.empty() ? nullptr : &tool_colors;
    ctxt.color_print_values = color_print_values.empty() ? nullptr : &color_print_values;
    ctxt.is_single_material_print = this->fff_print()->extruders().size()==1;
    ctxt.extruders_cnt = wxGetApp().extruders_edited_cnt();

    ctxt.shifted_copies = &print_object.instances();

    // order layers by print_z
    {
        size_t nlayers = 0;
        if (ctxt.has_perimeters || ctxt.has_infill)
            nlayers = print_object.layers().size();
        if (ctxt.has_support)
            nlayers += print_object.support_layers().size();
        ctxt.layers.reserve(nlayers);
    }
    if (ctxt.has_perimeters || ctxt.has_infill)
        for (const Layer *layer : print_object.layers())
            ctxt.layers.emplace_back(layer);
    if (ctxt.has_support)
        for (const Layer *layer : print_object.support_layers())
            ctxt.layers.emplace_back(layer);
    std::sort(ctxt.layers.begin(), ctxt.layers.end(), [](const Layer *l1, const Layer *l2) { return l1->print_z < l2->print_z; });

    // Maximum size of an allocation block: 32MB / sizeof(float)
    BOOST_LOG_TRIVIAL(debug) << "Loading print object toolpaths in parallel - start" << m_volumes.log_memory_info() << log_memory_info();

    const bool is_selected_separate_extruder = m_selected_extruder > 0 && ctxt.color_by_color_print();

    //FIXME Improve the heuristics for a grain size.
    size_t          grain_size = std::max(ctxt.layers.size() / 16, size_t(1));
    tbb::spin_mutex new_volume_mutex;
    auto            new_volume = [this, &new_volume_mutex](const float *color) -> GLVolume* {
    	// Allocate the volume before locking.
		GLVolume *volume = new GLVolume(color);
		volume->is_extrusion_path = true;
    	tbb::spin_mutex::scoped_lock lock;
    	// Lock by ROII, so if the emplace_back() fails, the lock will be released.
        lock.acquire(new_volume_mutex);
        m_volumes.volumes.emplace_back(volume);
        lock.release();
        return volume;
    };
    const size_t    volumes_cnt_initial = m_volumes.volumes.size();
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, ctxt.layers.size(), grain_size),
        [&ctxt, &new_volume, is_selected_separate_extruder, this](const tbb::blocked_range<size_t>& range) {
        GLVolumePtrs 		vols;
        std::vector<size_t>	color_print_layer_to_glvolume;
        auto                volume = [&ctxt, &vols, &color_print_layer_to_glvolume, &range](size_t layer_idx, int extruder, int feature) -> GLVolume& {            
            return *vols[ctxt.color_by_color_print()?
                ctxt.color_print_color_idx_by_layer_idx_and_extruder(layer_idx, extruder) :
				ctxt.color_by_tool() ? 
					std::min<int>(ctxt.number_tools() - 1, std::max<int>(extruder - 1, 0)) : 
					feature
				];
        };
        if (ctxt.color_by_color_print() || ctxt.color_by_tool()) {
            for (size_t i = 0; i < ctxt.number_tools(); ++i)
                vols.emplace_back(new_volume(ctxt.color_tool(i)));
        }
        else
            vols = { new_volume(ctxt.color_perimeters()), new_volume(ctxt.color_infill()), new_volume(ctxt.color_support()) };
        for (GLVolume *vol : vols)
			// Reserving number of vertices (3x position + 3x color)
        	vol->indexed_vertex_array.reserve(VERTEX_BUFFER_RESERVE_SIZE / 6);
        for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
            const Layer *layer = ctxt.layers[idx_layer];

            if (is_selected_separate_extruder)
            {
                bool at_least_one_has_correct_extruder = false;
                for (const LayerRegion* layerm : layer->regions())
                {
                    if (layerm->slices.surfaces.empty())
                        continue;
                    const PrintRegionConfig& cfg = layerm->region()->config();
                    if (cfg.perimeter_extruder.value    == m_selected_extruder ||
                        cfg.infill_extruder.value       == m_selected_extruder ||
                        cfg.solid_infill_extruder.value == m_selected_extruder ) {
                        at_least_one_has_correct_extruder = true;
                        break;
                    }
                }
                if (!at_least_one_has_correct_extruder)
                    continue;
            }

            for (GLVolume *vol : vols)
                if (vol->print_zs.empty() || vol->print_zs.back() != layer->print_z) {
                    vol->print_zs.emplace_back(layer->print_z);
                    vol->offsets.emplace_back(vol->indexed_vertex_array.quad_indices.size());
                    vol->offsets.emplace_back(vol->indexed_vertex_array.triangle_indices.size());
                }
            for (const PrintInstance &instance : *ctxt.shifted_copies) {
                const Point &copy = instance.shift;
                for (const LayerRegion *layerm : layer->regions()) {
                    if (is_selected_separate_extruder)
                    {
                        const PrintRegionConfig& cfg = layerm->region()->config();
                        if (cfg.perimeter_extruder.value    != m_selected_extruder ||
                            cfg.infill_extruder.value       != m_selected_extruder ||
                            cfg.solid_infill_extruder.value != m_selected_extruder)
                            continue;
                    }
                    if (ctxt.has_perimeters)
                        _3DScene::extrusionentity_to_verts(layerm->perimeters, float(layer->print_z), copy,
                        	volume(idx_layer, layerm->region()->config().perimeter_extruder.value, 0));
                    if (ctxt.has_infill) {
                        for (const ExtrusionEntity *ee : layerm->fills.entities) {
                            // fill represents infill extrusions of a single island.
                            const auto *fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                            if (! fill->entities.empty())
                                _3DScene::extrusionentity_to_verts(*fill, float(layer->print_z), copy,
	                                volume(idx_layer, 
		                                is_solid_infill(fill->entities.front()->role()) ?
			                                layerm->region()->config().solid_infill_extruder :
			                                layerm->region()->config().infill_extruder,
		                                1));
                        }
                    }
                }
                if (ctxt.has_support) {
                    const SupportLayer *support_layer = dynamic_cast<const SupportLayer*>(layer);
                    if (support_layer) {
                        for (const ExtrusionEntity *extrusion_entity : support_layer->support_fills.entities)
                            _3DScene::extrusionentity_to_verts(extrusion_entity, float(layer->print_z), copy,
	                            volume(idx_layer, 
		                            (extrusion_entity->role() == erSupportMaterial) ?
			                            support_layer->object()->config().support_material_extruder :
			                            support_layer->object()->config().support_material_interface_extruder,
		                            2));
                    }
                }
            }
            // Ensure that no volume grows over the limits. If the volume is too large, allocate a new one.
	        for (size_t i = 0; i < vols.size(); ++i) {
	            GLVolume &vol = *vols[i];
	            if (vol.indexed_vertex_array.vertices_and_normals_interleaved.size() > MAX_VERTEX_BUFFER_SIZE) {
	                vols[i] = new_volume(vol.color);
	                reserve_new_volume_finalize_old_volume(*vols[i], vol, false);
	            }
	        }
        }
        for (GLVolume *vol : vols)
        	// Ideally one would call vol->indexed_vertex_array.finalize() here to move the buffers to the OpenGL driver,
        	// but this code runs in parallel and the OpenGL driver is not thread safe.
            vol->indexed_vertex_array.shrink_to_fit();
    });

    BOOST_LOG_TRIVIAL(debug) << "Loading print object toolpaths in parallel - finalizing results" << m_volumes.log_memory_info() << log_memory_info();
    // Remove empty volumes from the newly added volumes.
    m_volumes.volumes.erase(
        std::remove_if(m_volumes.volumes.begin() + volumes_cnt_initial, m_volumes.volumes.end(),
        [](const GLVolume *volume) { return volume->empty(); }),
        m_volumes.volumes.end());
    for (size_t i = volumes_cnt_initial; i < m_volumes.volumes.size(); ++i)
        m_volumes.volumes[i]->indexed_vertex_array.finalize_geometry(m_initialized);

    BOOST_LOG_TRIVIAL(debug) << "Loading print object toolpaths in parallel - end" << m_volumes.log_memory_info() << log_memory_info();
}

void GLCanvas3D::_load_wipe_tower_toolpaths(const std::vector<std::string>& str_tool_colors)
{
    const Print *print = this->fff_print();
    if ((print == nullptr) || print->wipe_tower_data().tool_changes.empty())
        return;

    if (!print->is_step_done(psWipeTower))
        return;

    std::vector<float> tool_colors = _parse_colors(str_tool_colors);

    struct Ctxt
    {
        const Print                 *print;
        const std::vector<float>    *tool_colors;
        Vec2f                        wipe_tower_pos;
        float                        wipe_tower_angle;

        static const float*          color_support() { static float color[4] = { 0.5f, 1.0f, 0.5f, 1.f }; return color; } // greenish

        // For cloring by a tool, return a parsed color.
        bool                         color_by_tool() const { return tool_colors != nullptr; }
        size_t                       number_tools()  const { return this->color_by_tool() ? tool_colors->size() / 4 : 0; }
        const float*                 color_tool(size_t tool) const { return tool_colors->data() + tool * 4; }
        int                          volume_idx(int tool, int feature) const
        {
            return this->color_by_tool() ? std::min<int>(this->number_tools() - 1, std::max<int>(tool, 0)) : feature;
        }

        const std::vector<WipeTower::ToolChangeResult>& tool_change(size_t idx) {
            const auto &tool_changes = print->wipe_tower_data().tool_changes;
            return priming.empty() ?
                ((idx == tool_changes.size()) ? final : tool_changes[idx]) :
                ((idx == 0) ? priming : (idx == tool_changes.size() + 1) ? final : tool_changes[idx - 1]);
        }
        std::vector<WipeTower::ToolChangeResult> priming;
        std::vector<WipeTower::ToolChangeResult> final;
    } ctxt;

    ctxt.print = print;
    ctxt.tool_colors = tool_colors.empty() ? nullptr : &tool_colors;
    if (print->wipe_tower_data().priming && print->config().single_extruder_multi_material_priming)
        for (int i=0; i<(int)print->wipe_tower_data().priming.get()->size(); ++i)
            ctxt.priming.emplace_back(print->wipe_tower_data().priming.get()->at(i));
    if (print->wipe_tower_data().final_purge)
        ctxt.final.emplace_back(*print->wipe_tower_data().final_purge.get());

    ctxt.wipe_tower_angle = ctxt.print->config().wipe_tower_rotation_angle.value/180.f * PI;
    ctxt.wipe_tower_pos = Vec2f(ctxt.print->config().wipe_tower_x.value, ctxt.print->config().wipe_tower_y.value);

    BOOST_LOG_TRIVIAL(debug) << "Loading wipe tower toolpaths in parallel - start" << m_volumes.log_memory_info() << log_memory_info();

    //FIXME Improve the heuristics for a grain size.
    size_t          n_items = print->wipe_tower_data().tool_changes.size() + (ctxt.priming.empty() ? 0 : 1);
    size_t          grain_size = std::max(n_items / 128, size_t(1));
    tbb::spin_mutex new_volume_mutex;
    auto            new_volume = [this, &new_volume_mutex](const float *color) -> GLVolume* {
        auto *volume = new GLVolume(color);
		volume->is_extrusion_path = true;
        tbb::spin_mutex::scoped_lock lock;
        lock.acquire(new_volume_mutex);
        m_volumes.volumes.emplace_back(volume);
        lock.release();
        return volume;
    };
    const size_t   volumes_cnt_initial = m_volumes.volumes.size();
    std::vector<GLVolumeCollection> volumes_per_thread(n_items);
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, n_items, grain_size),
        [&ctxt, &new_volume](const tbb::blocked_range<size_t>& range) {
        // Bounding box of this slab of a wipe tower.
        GLVolumePtrs vols;
        if (ctxt.color_by_tool()) {
            for (size_t i = 0; i < ctxt.number_tools(); ++i)
                vols.emplace_back(new_volume(ctxt.color_tool(i)));
        }
        else
            vols = { new_volume(ctxt.color_support()) };
        for (GLVolume *volume : vols)
			// Reserving number of vertices (3x position + 3x color)
            volume->indexed_vertex_array.reserve(VERTEX_BUFFER_RESERVE_SIZE / 6);
        for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++idx_layer) {
            const std::vector<WipeTower::ToolChangeResult> &layer = ctxt.tool_change(idx_layer);
            for (size_t i = 0; i < vols.size(); ++i) {
                GLVolume &vol = *vols[i];
                if (vol.print_zs.empty() || vol.print_zs.back() != layer.front().print_z) {
                    vol.print_zs.emplace_back(layer.front().print_z);
                    vol.offsets.emplace_back(vol.indexed_vertex_array.quad_indices.size());
                    vol.offsets.emplace_back(vol.indexed_vertex_array.triangle_indices.size());
                }
            }
            for (const WipeTower::ToolChangeResult &extrusions : layer) {
                for (size_t i = 1; i < extrusions.extrusions.size();) {
                    const WipeTower::Extrusion &e = extrusions.extrusions[i];
                    if (e.width == 0.) {
                        ++i;
                        continue;
                    }
                    size_t j = i + 1;
                    if (ctxt.color_by_tool())
                        for (; j < extrusions.extrusions.size() && extrusions.extrusions[j].tool == e.tool && extrusions.extrusions[j].width > 0.f; ++j);
                    else
                        for (; j < extrusions.extrusions.size() && extrusions.extrusions[j].width > 0.f; ++j);
                    size_t              n_lines = j - i;
                    Lines               lines;
                    std::vector<double> widths;
                    std::vector<double> heights;
                    lines.reserve(n_lines);
                    widths.reserve(n_lines);
                    heights.assign(n_lines, extrusions.layer_height);
                    WipeTower::Extrusion e_prev = extrusions.extrusions[i-1];

                    if (!extrusions.priming) { // wipe tower extrusions describe the wipe tower at the origin with no rotation
                        e_prev.pos = Eigen::Rotation2Df(ctxt.wipe_tower_angle) * e_prev.pos;
                        e_prev.pos += ctxt.wipe_tower_pos;
                    }

                    for (; i < j; ++i) {
                        WipeTower::Extrusion e = extrusions.extrusions[i];
                        assert(e.width > 0.f);
                        if (!extrusions.priming) {
                            e.pos = Eigen::Rotation2Df(ctxt.wipe_tower_angle) * e.pos;
                            e.pos += ctxt.wipe_tower_pos;
                        }

                        lines.emplace_back(Point::new_scale(e_prev.pos.x(), e_prev.pos.y()), Point::new_scale(e.pos.x(), e.pos.y()));
                        widths.emplace_back(e.width);

                        e_prev = e;
                    }
                    _3DScene::thick_lines_to_verts(lines, widths, heights, lines.front().a == lines.back().b, extrusions.print_z,
                        *vols[ctxt.volume_idx(e.tool, 0)]);
                }
            }
        }
        for (size_t i = 0; i < vols.size(); ++i) {
            GLVolume &vol = *vols[i];
            if (vol.indexed_vertex_array.vertices_and_normals_interleaved.size() > MAX_VERTEX_BUFFER_SIZE) {
                vols[i] = new_volume(vol.color);
                reserve_new_volume_finalize_old_volume(*vols[i], vol, false);
            }
        }
        for (GLVolume *vol : vols)
            vol->indexed_vertex_array.shrink_to_fit();
    });

    BOOST_LOG_TRIVIAL(debug) << "Loading wipe tower toolpaths in parallel - finalizing results" << m_volumes.log_memory_info() << log_memory_info();
    // Remove empty volumes from the newly added volumes.
    m_volumes.volumes.erase(
        std::remove_if(m_volumes.volumes.begin() + volumes_cnt_initial, m_volumes.volumes.end(),
        [](const GLVolume *volume) { return volume->empty(); }),
        m_volumes.volumes.end());
    for (size_t i = volumes_cnt_initial; i < m_volumes.volumes.size(); ++i)
        m_volumes.volumes[i]->indexed_vertex_array.finalize_geometry(m_initialized);

    BOOST_LOG_TRIVIAL(debug) << "Loading wipe tower toolpaths in parallel - end" << m_volumes.log_memory_info() << log_memory_info();
}

static inline int hex_digit_to_int(const char c)
{
    return
        (c >= '0' && c <= '9') ? int(c - '0') :
        (c >= 'A' && c <= 'F') ? int(c - 'A') + 10 :
        (c >= 'a' && c <= 'f') ? int(c - 'a') + 10 : -1;
}

void GLCanvas3D::_load_gcode_extrusion_paths(const GCodePreviewData& preview_data, const std::vector<float>& tool_colors)
{
    BOOST_LOG_TRIVIAL(debug) << "Loading G-code extrusion paths - start" << m_volumes.log_memory_info() << log_memory_info();

    // helper functions to select data in dependence of the extrusion view type
    struct Helper
    {
        static float path_filter(GCodePreviewData::Extrusion::EViewType type, const GCodePreviewData::Extrusion::Path& path)
        {
            switch (type)
            {
            case GCodePreviewData::Extrusion::FeatureType:
            	// The role here is used for coloring.
                return (float)path.extrusion_role;
            case GCodePreviewData::Extrusion::Height:
                return path.height;
            case GCodePreviewData::Extrusion::Width:
                return path.width;
            case GCodePreviewData::Extrusion::Feedrate:
                return path.feedrate;
            case GCodePreviewData::Extrusion::FanSpeed:
                return path.fan_speed;
            case GCodePreviewData::Extrusion::VolumetricRate:
                return path.feedrate * path.mm3_per_mm;
            case GCodePreviewData::Extrusion::Tool:
                return (float)path.extruder_id;
            case GCodePreviewData::Extrusion::ColorPrint:
                return (float)path.cp_color_id;
            default:
                return 0.0f;
            }

            return 0.0f;
        }

        static Color path_color(const GCodePreviewData& data, const std::vector<float>& tool_colors, float value)
        {
            switch (data.extrusion.view_type)
            {
            case GCodePreviewData::Extrusion::FeatureType:
                return data.get_extrusion_role_color((ExtrusionRole)(int)value);
            case GCodePreviewData::Extrusion::Height:
                return data.get_height_color(value);
            case GCodePreviewData::Extrusion::Width:
                return data.get_width_color(value);
            case GCodePreviewData::Extrusion::Feedrate:
                return data.get_feedrate_color(value);
            case GCodePreviewData::Extrusion::FanSpeed:
                return data.get_fan_speed_color(value);
            case GCodePreviewData::Extrusion::VolumetricRate:
                return data.get_volumetric_rate_color(value);
            case GCodePreviewData::Extrusion::Tool:
            {
                Color color;
                ::memcpy((void*)color.rgba.data(), (const void*)(tool_colors.data() + (unsigned int)value * 4), 4 * sizeof(float));
                return color;
            }
            case GCodePreviewData::Extrusion::ColorPrint:
            {
                int color_cnt = (int)tool_colors.size() / 4;
                int val = value > color_cnt ? color_cnt - 1 : value;

                Color color;
                ::memcpy((void*)color.rgba.data(), (const void*)(tool_colors.data() + val * 4), 4 * sizeof(float));

                return color;
            }
            default:
                return Color{};
            }

            return Color{};
        }
    };

    size_t initial_volumes_count = m_volumes.volumes.size();
    size_t initial_volume_index_count = m_gcode_preview_volume_index.first_volumes.size();

    try 
    {
	    BOOST_LOG_TRIVIAL(debug) << "Loading G-code extrusion paths - create volumes" << m_volumes.log_memory_info() << log_memory_info();

	    // detects filters
	    size_t vertex_buffer_prealloc_size = 0;
	    std::vector<std::vector<std::pair<float, GLVolume*>>> roles_filters;
	    {
		    std::vector<size_t> num_paths_per_role(size_t(erCount), 0);
		    for (const GCodePreviewData::Extrusion::Layer &layer : preview_data.extrusion.layers)
		        for (const GCodePreviewData::Extrusion::Path &path : layer.paths)
		        	++ num_paths_per_role[size_t(path.extrusion_role)];
            std::vector<std::vector<float>> roles_values;
			roles_values.assign(size_t(erCount), std::vector<float>());
		    for (size_t i = 0; i < roles_values.size(); ++ i)
		    	roles_values[i].reserve(num_paths_per_role[i]);
            for (const GCodePreviewData::Extrusion::Layer& layer : preview_data.extrusion.layers)
		        for (const GCodePreviewData::Extrusion::Path &path : layer.paths)
		        	roles_values[size_t(path.extrusion_role)].emplace_back(Helper::path_filter(preview_data.extrusion.view_type, path));
            roles_filters.reserve(size_t(erCount));
			size_t num_buffers = 0;
		    for (std::vector<float> &values : roles_values) {
		    	sort_remove_duplicates(values);
		    	num_buffers += values.size();
		    }
		    if (num_buffers == 0)
			    // nothing to render, return
		        return;
		    vertex_buffer_prealloc_size = (uint64_t(num_buffers) * uint64_t(VERTEX_BUFFER_RESERVE_SIZE) < VERTEX_BUFFER_RESERVE_SIZE_SUM_MAX) ? 
	    		VERTEX_BUFFER_RESERVE_SIZE : next_highest_power_of_2(VERTEX_BUFFER_RESERVE_SIZE_SUM_MAX / num_buffers) / 2;
		    for (std::vector<float> &values : roles_values) {
		    	size_t role = &values - &roles_values.front();
				roles_filters.emplace_back();
		    	if (! values.empty()) {
		        	m_gcode_preview_volume_index.first_volumes.emplace_back(GCodePreviewVolumeIndex::Extrusion, role, (unsigned int)m_volumes.volumes.size());
					for (const float value : values)
						roles_filters.back().emplace_back(value, m_volumes.new_toolpath_volume(Helper::path_color(preview_data, tool_colors, value).rgba.data(), vertex_buffer_prealloc_size));
				}
			}
		}

	    BOOST_LOG_TRIVIAL(debug) << "Loading G-code extrusion paths - populate volumes" << m_volumes.log_memory_info() << log_memory_info();

	    // populates volumes
        const bool is_selected_separate_extruder = m_selected_extruder > 0 && preview_data.extrusion.view_type == GCodePreviewData::Extrusion::ColorPrint;
		for (const GCodePreviewData::Extrusion::Layer& layer : preview_data.extrusion.layers)
		{
			for (const GCodePreviewData::Extrusion::Path& path : layer.paths)
			{
                if (is_selected_separate_extruder && path.extruder_id != m_selected_extruder - 1)
                    continue;
				std::vector<std::pair<float, GLVolume*>> &filters = roles_filters[size_t(path.extrusion_role)];
				auto key = std::make_pair<float, GLVolume*>(Helper::path_filter(preview_data.extrusion.view_type, path), nullptr);
				auto it_filter = std::lower_bound(filters.begin(), filters.end(), key);
				assert(it_filter != filters.end() && key.first == it_filter->first);

				GLVolume& vol = *it_filter->second;
				vol.print_zs.emplace_back(layer.z);
				vol.offsets.emplace_back(vol.indexed_vertex_array.quad_indices.size());
				vol.offsets.emplace_back(vol.indexed_vertex_array.triangle_indices.size());

				_3DScene::extrusionentity_to_verts(path.polyline, path.width, path.height, layer.z, vol);
			}
			// Ensure that no volume grows over the limits. If the volume is too large, allocate a new one.
		    for (std::vector<std::pair<float, GLVolume*>> &filters : roles_filters) {
		    	unsigned int role = (unsigned int)(&filters - &roles_filters.front());
			    for (std::pair<float, GLVolume*> &filter : filters)
					if (filter.second->indexed_vertex_array.vertices_and_normals_interleaved.size() > MAX_VERTEX_BUFFER_SIZE) {
						if (m_gcode_preview_volume_index.first_volumes.back().type != GCodePreviewVolumeIndex::Extrusion || m_gcode_preview_volume_index.first_volumes.back().flag != role)
			        		m_gcode_preview_volume_index.first_volumes.emplace_back(GCodePreviewVolumeIndex::Extrusion, role, (unsigned int)m_volumes.volumes.size());
						GLVolume& vol = *filter.second;
						filter.second = m_volumes.new_toolpath_volume(vol.color);
						reserve_new_volume_finalize_old_volume(*filter.second, vol, m_initialized, vertex_buffer_prealloc_size);
					}
		    }
	    }

	    // Finalize volumes and sends geometry to gpu
	    for (std::vector<std::pair<float, GLVolume*>> &filters : roles_filters)
		    for (std::pair<float, GLVolume*> &filter : filters)
	    		filter.second->indexed_vertex_array.finalize_geometry(m_initialized);

	    BOOST_LOG_TRIVIAL(debug) << "Loading G-code extrusion paths - end" << m_volumes.log_memory_info() << log_memory_info();
	} 
	catch (const std::bad_alloc & /* err */)
	{
        // an error occourred - restore to previous state and return
        GLVolumePtrs::iterator begin = m_volumes.volumes.begin() + initial_volumes_count;
        GLVolumePtrs::iterator end = m_volumes.volumes.end();
        for (GLVolumePtrs::iterator it = begin; it < end; ++it)
            delete *it;
        m_volumes.volumes.erase(begin, end);
        m_gcode_preview_volume_index.first_volumes.erase(m_gcode_preview_volume_index.first_volumes.begin() + initial_volume_index_count, m_gcode_preview_volume_index.first_volumes.end());
	    BOOST_LOG_TRIVIAL(debug) << "Loading G-code extrusion paths - failed on low memory" << m_volumes.log_memory_info() << log_memory_info();
        //FIXME rethrow bad_alloc?
	}
}

template<typename TYPE, typename FUNC_VALUE, typename FUNC_COLOR>
inline void travel_paths_internal(
	// input
	const GCodePreviewData &preview_data, 
	// accessors
	FUNC_VALUE func_value, FUNC_COLOR func_color,
	// output
	GLVolumeCollection &volumes, bool gl_initialized)

{
	// colors travels by type
	std::vector<std::pair<TYPE, GLVolume*>> by_type;
	{
		std::vector<TYPE> values;
		values.reserve(preview_data.travel.polylines.size());
		for (const GCodePreviewData::Travel::Polyline& polyline : preview_data.travel.polylines)
			values.emplace_back(func_value(polyline));
		sort_remove_duplicates(values);
		by_type.reserve(values.size());
		// creates a new volume for each feedrate
		for (TYPE type : values)
			by_type.emplace_back(type, volumes.new_nontoolpath_volume(func_color(type).rgba.data(), VERTEX_BUFFER_RESERVE_SIZE));
	}

	// populates volumes
	std::pair<TYPE, GLVolume*> key(0.f, nullptr);
	for (const GCodePreviewData::Travel::Polyline& polyline : preview_data.travel.polylines)
	{
		key.first = func_value(polyline);
		auto it = std::lower_bound(by_type.begin(), by_type.end(), key, [](const std::pair<TYPE, GLVolume*>& l, const std::pair<TYPE, GLVolume*>& r) { return l.first < r.first; });
		assert(it != by_type.end() && it->first == func_value(polyline));

		GLVolume& vol = *it->second;
		vol.print_zs.emplace_back(unscale<double>(polyline.polyline.bounding_box().min(2)));
		vol.offsets.emplace_back(vol.indexed_vertex_array.quad_indices.size());
		vol.offsets.emplace_back(vol.indexed_vertex_array.triangle_indices.size());

		_3DScene::polyline3_to_verts(polyline.polyline, preview_data.travel.width, preview_data.travel.height, vol);

		// Ensure that no volume grows over the limits. If the volume is too large, allocate a new one.
		if (vol.indexed_vertex_array.vertices_and_normals_interleaved.size() > MAX_VERTEX_BUFFER_SIZE) {
			it->second = volumes.new_nontoolpath_volume(vol.color);
			reserve_new_volume_finalize_old_volume(*it->second, vol, gl_initialized);
		}
	}

	for (auto &feedrate : by_type)
		feedrate.second->finalize_geometry(gl_initialized);
}

void GLCanvas3D::_load_gcode_travel_paths(const GCodePreviewData& preview_data, const std::vector<float>& tool_colors)
{
	// nothing to render, return
	if (preview_data.travel.polylines.empty())
		return;

    size_t initial_volumes_count = m_volumes.volumes.size();
    size_t volume_index_allocated = false;

    try {
    	m_gcode_preview_volume_index.first_volumes.emplace_back(GCodePreviewVolumeIndex::Travel, 0, (unsigned int)initial_volumes_count);
    	volume_index_allocated = true;

	    switch (preview_data.extrusion.view_type)
	    {
	    case GCodePreviewData::Extrusion::Feedrate:
			travel_paths_internal<float>(preview_data,
				[](const GCodePreviewData::Travel::Polyline &polyline) { return polyline.feedrate; }, 
				[&preview_data](const float feedrate) -> const Color { return preview_data.get_feedrate_color(feedrate); },
				m_volumes, m_initialized);
	        break;
	    case GCodePreviewData::Extrusion::Tool:
	    	travel_paths_internal<unsigned int>(preview_data,
				[](const GCodePreviewData::Travel::Polyline &polyline) { return polyline.extruder_id; }, 
				[&tool_colors](const unsigned int extruder_id) -> const Color { assert((extruder_id + 1) * 4 <= tool_colors.size()); return Color(tool_colors.data() + extruder_id * 4); },
				m_volumes, m_initialized);
	        break;
	    default:
	    	travel_paths_internal<unsigned int>(preview_data,
				[](const GCodePreviewData::Travel::Polyline &polyline) { return polyline.type; }, 
				[&preview_data](const unsigned int type) -> const Color& { return preview_data.travel.type_colors[type]; },
				m_volumes, m_initialized);
	        break;
	    }
	} catch (const std::bad_alloc & /* ex */) {
        // an error occourred - restore to previous state and return
        GLVolumePtrs::iterator begin = m_volumes.volumes.begin() + initial_volumes_count;
        GLVolumePtrs::iterator end   = m_volumes.volumes.end();
        for (GLVolumePtrs::iterator it = begin; it < end; ++it)
            delete *it;
        m_volumes.volumes.erase(begin, end);
        if (volume_index_allocated)
        	m_gcode_preview_volume_index.first_volumes.pop_back();
        //FIXME report the memory issue?
    }
}

void GLCanvas3D::_load_fff_shells()
{
    size_t initial_volumes_count = m_volumes.volumes.size();
    m_gcode_preview_volume_index.first_volumes.emplace_back(GCodePreviewVolumeIndex::Shell, 0, (unsigned int)initial_volumes_count);

    const Print *print = this->fff_print();
    if (print->objects().empty())
        // nothing to render, return
        return;

    // adds objects' volumes 
    int object_id = 0;
    for (const PrintObject* obj : print->objects())
    {
        const ModelObject* model_obj = obj->model_object();

        std::vector<int> instance_ids(model_obj->instances.size());
        for (int i = 0; i < (int)model_obj->instances.size(); ++i)
        {
            instance_ids[i] = i;
        }

        m_volumes.load_object(model_obj, object_id, instance_ids, "object", m_initialized);

        ++object_id;
    }

    if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF) {
        // adds wipe tower's volume
        double max_z = print->objects()[0]->model_object()->get_model()->bounding_box().max(2);
        const PrintConfig& config = print->config();
        size_t extruders_count = config.nozzle_diameter.size();
        if ((extruders_count > 1) && config.wipe_tower && !config.complete_objects) {

            const DynamicPrintConfig &print_config  = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            double layer_height                     = print_config.opt_float("layer_height");
            double first_layer_height               = print_config.get_abs_value("first_layer_height", layer_height);
            double nozzle_diameter                  = print->config().nozzle_diameter.values[0];
            float depth = print->wipe_tower_data(extruders_count, first_layer_height, nozzle_diameter).depth;
            float brim_width = print->wipe_tower_data(extruders_count, first_layer_height, nozzle_diameter).brim_width;

            m_volumes.load_wipe_tower_preview(1000, config.wipe_tower_x, config.wipe_tower_y, config.wipe_tower_width, depth, max_z, config.wipe_tower_rotation_angle,
                !print->is_step_done(psWipeTower), brim_width, m_initialized);
        }
    }
}

// While it looks like we can call 
// this->reload_scene(true, true)
// the two functions are quite different:
// 1) This function only loads objects, for which the step slaposSliceSupports already finished. Therefore objects outside of the print bed never load.
// 2) This function loads object mesh with the relative scaling correction (the "relative_correction" parameter) was applied,
// 	  therefore the mesh may be slightly larger or smaller than the mesh shown in the 3D scene.
void GLCanvas3D::_load_sla_shells()
{
    const SLAPrint* print = this->sla_print();
    if (print->objects().empty())
        // nothing to render, return
        return;

    auto add_volume = [this](const SLAPrintObject &object, int volume_id, const SLAPrintObject::Instance& instance,
        const TriangleMesh &mesh, const float color[4], bool outside_printer_detection_enabled) {
        m_volumes.volumes.emplace_back(new GLVolume(color));
        GLVolume& v = *m_volumes.volumes.back();
        v.indexed_vertex_array.load_mesh(mesh);
        v.indexed_vertex_array.finalize_geometry(this->m_initialized);
        v.shader_outside_printer_detection_enabled = outside_printer_detection_enabled;
        v.composite_id.volume_id = volume_id;
        v.set_instance_offset(unscale(instance.shift.x(), instance.shift.y(), 0));
        v.set_instance_rotation(Vec3d(0.0, 0.0, (double)instance.rotation));
        v.set_instance_mirror(X, object.is_left_handed() ? -1. : 1.);
        v.set_convex_hull(mesh.convex_hull_3d());
    };

    // adds objects' volumes 
    for (const SLAPrintObject* obj : print->objects())
        if (obj->is_step_done(slaposSliceSupports)) {
            unsigned int initial_volumes_count = (unsigned int)m_volumes.volumes.size();
            for (const SLAPrintObject::Instance& instance : obj->instances()) {
                add_volume(*obj, 0, instance, obj->get_mesh_to_print(), GLVolume::MODEL_COLOR[0], true);
                // Set the extruder_id and volume_id to achieve the same color as in the 3D scene when
                // through the update_volumes_colors_by_extruder() call.
                m_volumes.volumes.back()->extruder_id = obj->model_object()->volumes.front()->extruder_id();
                if (obj->is_step_done(slaposSupportTree) && obj->has_mesh(slaposSupportTree))
                    add_volume(*obj, -int(slaposSupportTree), instance, obj->support_mesh(), GLVolume::SLA_SUPPORT_COLOR, true);
                if (obj->is_step_done(slaposPad) && obj->has_mesh(slaposPad))
                    add_volume(*obj, -int(slaposPad), instance, obj->pad_mesh(), GLVolume::SLA_PAD_COLOR, false);
            }
            double shift_z = obj->get_current_elevation();
            for (unsigned int i = initial_volumes_count; i < m_volumes.volumes.size(); ++ i) {
                GLVolume& v = *m_volumes.volumes[i];
                // apply shift z
                v.set_sla_shift_z(shift_z);
            }
        }

    update_volumes_colors_by_extruder();
}

void GLCanvas3D::_update_gcode_volumes_visibility(const GCodePreviewData& preview_data)
{
    unsigned int size = (unsigned int)m_gcode_preview_volume_index.first_volumes.size();
    for (unsigned int i = 0; i < size; ++i)
    {
        GLVolumePtrs::iterator begin = m_volumes.volumes.begin() + m_gcode_preview_volume_index.first_volumes[i].id;
        GLVolumePtrs::iterator end = (i + 1 < size) ? m_volumes.volumes.begin() + m_gcode_preview_volume_index.first_volumes[i + 1].id : m_volumes.volumes.end();

        for (GLVolumePtrs::iterator it = begin; it != end; ++it)
        {
            GLVolume* volume = *it;

            switch (m_gcode_preview_volume_index.first_volumes[i].type)
            {
            case GCodePreviewVolumeIndex::Extrusion:
            {
                if ((ExtrusionRole)m_gcode_preview_volume_index.first_volumes[i].flag == erCustom)
                    volume->zoom_to_volumes = false;

                volume->is_active = preview_data.extrusion.is_role_flag_set((ExtrusionRole)m_gcode_preview_volume_index.first_volumes[i].flag);
                break;
            }
            case GCodePreviewVolumeIndex::Travel:
            {
                volume->is_active = preview_data.travel.is_visible;
                volume->zoom_to_volumes = false;
                break;
            }
            case GCodePreviewVolumeIndex::Retraction:
            {
                volume->is_active = preview_data.retraction.is_visible;
                volume->zoom_to_volumes = false;
                break;
            }
            case GCodePreviewVolumeIndex::Unretraction:
            {
                volume->is_active = preview_data.unretraction.is_visible;
                volume->zoom_to_volumes = false;
                break;
            }
            case GCodePreviewVolumeIndex::Shell:
            {
                volume->is_active = preview_data.shell.is_visible;
                volume->color[3] = 0.25f;
                volume->zoom_to_volumes = false;
                break;
            }
            default:
            {
                volume->is_active = false;
                volume->zoom_to_volumes = false;
                break;
            }
            }
        }
    }
}

void GLCanvas3D::_update_toolpath_volumes_outside_state()
{
    // tolerance to avoid false detection at bed edges
    static const double tolerance_x = 0.05;
    static const double tolerance_y = 0.05;

    BoundingBoxf3 print_volume;
    if (m_config != nullptr)
    {
        const ConfigOptionPoints* opt = dynamic_cast<const ConfigOptionPoints*>(m_config->option("bed_shape"));
        if (opt != nullptr)
        {
            BoundingBox bed_box_2D = get_extents(Polygon::new_scale(opt->values));
            print_volume = BoundingBoxf3(Vec3d(unscale<double>(bed_box_2D.min(0)) - tolerance_x, unscale<double>(bed_box_2D.min(1)) - tolerance_y, 0.0), Vec3d(unscale<double>(bed_box_2D.max(0)) + tolerance_x, unscale<double>(bed_box_2D.max(1)) + tolerance_y, m_config->opt_float("max_print_height")));
            // Allow the objects to protrude below the print bed
            print_volume.min(2) = -1e10;
        }
    }

    for (GLVolume* volume : m_volumes.volumes)
    {
        volume->is_outside = ((print_volume.radius() > 0.0) && volume->is_extrusion_path) ? !print_volume.contains(volume->bounding_box()) : false;
    }
}

void GLCanvas3D::_update_sla_shells_outside_state()
{
    // tolerance to avoid false detection at bed edges
    static const double tolerance_x = 0.05;
    static const double tolerance_y = 0.05;

    BoundingBoxf3 print_volume;
    if (m_config != nullptr)
    {
        const ConfigOptionPoints* opt = dynamic_cast<const ConfigOptionPoints*>(m_config->option("bed_shape"));
        if (opt != nullptr)
        {
            BoundingBox bed_box_2D = get_extents(Polygon::new_scale(opt->values));
            print_volume = BoundingBoxf3(Vec3d(unscale<double>(bed_box_2D.min(0)) - tolerance_x, unscale<double>(bed_box_2D.min(1)) - tolerance_y, 0.0), Vec3d(unscale<double>(bed_box_2D.max(0)) + tolerance_x, unscale<double>(bed_box_2D.max(1)) + tolerance_y, m_config->opt_float("max_print_height")));
            // Allow the objects to protrude below the print bed
            print_volume.min(2) = -1e10;
        }
    }

    for (GLVolume* volume : m_volumes.volumes)
    {
        volume->is_outside = ((print_volume.radius() > 0.0) && volume->shader_outside_printer_detection_enabled) ? !print_volume.contains(volume->transformed_convex_hull_bounding_box()) : false;
    }
}

void GLCanvas3D::_show_warning_texture_if_needed(WarningTexture::Warning warning)
{
    _set_current();
    _set_warning_texture(warning, _is_any_volume_outside());
}

std::vector<float> GLCanvas3D::_parse_colors(const std::vector<std::string>& colors)
{
    static const float INV_255 = 1.0f / 255.0f;

    std::vector<float> output(colors.size() * 4, 1.0f);
    for (size_t i = 0; i < colors.size(); ++i)
    {
        const std::string& color = colors[i];
        const char* c = color.data() + 1;
        if ((color.size() == 7) && (color.front() == '#'))
        {
            for (size_t j = 0; j < 3; ++j)
            {
                int digit1 = hex_digit_to_int(*c++);
                int digit2 = hex_digit_to_int(*c++);
                if ((digit1 == -1) || (digit2 == -1))
                    break;

                output[i * 4 + j] = float(digit1 * 16 + digit2) * INV_255;
            }
        }
    }
    return output;
}

void GLCanvas3D::_generate_legend_texture(const GCodePreviewData& preview_data, const std::vector<float>& tool_colors)
{
    m_legend_texture.generate(preview_data, tool_colors, *this, true);
}

void GLCanvas3D::_set_warning_texture(WarningTexture::Warning warning, bool state)
{
    m_warning_texture.activate(warning, state, *this);
}

bool GLCanvas3D::_is_any_volume_outside() const
{
    for (const GLVolume* volume : m_volumes.volumes)
    {
        if ((volume != nullptr) && volume->is_outside)
            return true;
    }

    return false;
}

void GLCanvas3D::_update_selection_from_hover()
{
    bool ctrl_pressed = wxGetKeyState(WXK_CONTROL);

    if (m_hover_volume_idxs.empty())
    {
        if (!ctrl_pressed && (m_rectangle_selection.get_state() == GLSelectionRectangle::Select))
            m_selection.remove_all();

        return;
    }

    GLSelectionRectangle::EState state = m_rectangle_selection.get_state();

    bool hover_modifiers_only = true;
    for (int i : m_hover_volume_idxs)
    {
        if (!m_volumes.volumes[i]->is_modifier)
        {
            hover_modifiers_only = false;
            break;
        }
    }

    bool selection_changed = false;
    if (state == GLSelectionRectangle::Select)
    {
        bool contains_all = true;
        for (int i : m_hover_volume_idxs)
        {
            if (!m_selection.contains_volume((unsigned int)i))
            {
                contains_all = false;
                break;
            }
        }

        // the selection is going to be modified (Add)
        if (!contains_all)
        {
            wxGetApp().plater()->take_snapshot(_(L("Selection-Add from rectangle")));
            selection_changed = true;
        }
    }
    else
    {
        bool contains_any = false;
        for (int i : m_hover_volume_idxs)
        {
            if (m_selection.contains_volume((unsigned int)i))
            {
                contains_any = true;
                break;
            }
        }

        // the selection is going to be modified (Remove)
        if (contains_any)
        {
            wxGetApp().plater()->take_snapshot(_(L("Selection-Remove from rectangle")));
            selection_changed = true;
        }
    }

    if (!selection_changed)
        return;

    Plater::SuppressSnapshots suppress(wxGetApp().plater());

    if ((state == GLSelectionRectangle::Select) && !ctrl_pressed)
        m_selection.clear();

    for (int i : m_hover_volume_idxs)
    {
        if (state == GLSelectionRectangle::Select)
        {
            if (hover_modifiers_only)
            {
                const GLVolume& v = *m_volumes.volumes[i];
                m_selection.add_volume(v.object_idx(), v.volume_idx(), v.instance_idx(), false);
            }
            else
                m_selection.add(i, false);
        }
        else
            m_selection.remove(i);
    }

    if (m_selection.is_empty())
        m_gizmos.reset_all_states();
    else
        m_gizmos.refresh_on_off_state();

    m_gizmos.update_data();
    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
    m_dirty = true;
}

bool GLCanvas3D::_deactivate_undo_redo_toolbar_items()
{
    if (m_undoredo_toolbar.is_item_pressed("undo"))
    {
        m_undoredo_toolbar.force_right_action(m_undoredo_toolbar.get_item_id("undo"), *this);
        return true;
    }
    else if (m_undoredo_toolbar.is_item_pressed("redo"))
    {
        m_undoredo_toolbar.force_right_action(m_undoredo_toolbar.get_item_id("redo"), *this);
        return true;
    }

    return false;
}

bool GLCanvas3D::_deactivate_search_toolbar_item()
{
    if (m_main_toolbar.is_item_pressed("search"))
    {
        m_main_toolbar.force_left_action(m_main_toolbar.get_item_id("search"), *this);
        return true;
    }

    return false;
}

bool GLCanvas3D::_activate_search_toolbar_item()
{
    if (!m_main_toolbar.is_item_pressed("search"))
    {
        m_main_toolbar.force_left_action(m_main_toolbar.get_item_id("search"), *this);
        return true;
    }

    return false;
}

bool GLCanvas3D::_deactivate_collapse_toolbar_items()
{
    if (m_collapse_toolbar.is_item_pressed("print"))
    {
        m_collapse_toolbar.force_left_action(m_collapse_toolbar.get_item_id("print"), *this);
        return true;
    }

    return false;
}

const Print* GLCanvas3D::fff_print() const
{
    return (m_process == nullptr) ? nullptr : m_process->fff_print();
}

const SLAPrint* GLCanvas3D::sla_print() const
{
    return (m_process == nullptr) ? nullptr : m_process->sla_print();
}

void GLCanvas3D::WipeTowerInfo::apply_wipe_tower() const
{
    DynamicPrintConfig cfg;
    cfg.opt<ConfigOptionFloat>("wipe_tower_x", true)->value = m_pos(X);
    cfg.opt<ConfigOptionFloat>("wipe_tower_y", true)->value = m_pos(Y);
    cfg.opt<ConfigOptionFloat>("wipe_tower_rotation_angle", true)->value = (180./M_PI) * m_rotation;
    wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(cfg);
}

} // namespace GUI
} // namespace Slic3r
