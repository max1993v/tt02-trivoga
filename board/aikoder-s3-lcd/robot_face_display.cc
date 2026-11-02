#include "robot_face_display.h"
#include "audio_codec.h"
#include "application.h"
#include "device_state.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cstring>
#include <cmath>

#define TAG "RobotFaceDisplay"

// Layout (tela 296x240; coordenadas ABSOLUTAS, iguais às do editor de
// expressões "estilo sólido" - olho preenchido + brilho). Centro: (148, 120).
static constexpr int kCx = 148, kCy = 120;
static constexpr int kEyeW = 64, kEyeH = 64;
static constexpr int kEyeWAttentive = 76, kEyeHAttentive = 76;
static constexpr int kEyeCxL = 98, kEyeCxR = 198, kEyeCy = 112;
static constexpr int kPupilR = 9;  // raio padrão do brilho (glint) no olho neutro
static constexpr int kZzzOffsetX = 65, kZzzOffsetY = -78;
static constexpr int kGazeMaxDx = 12, kGazeMaxDy = 8;
static constexpr int kBrowW = 8;   // espessura das linhas de sobrancelha/boca
static constexpr int kMouthLineW = 8;

// Esquema de cores: dark_theme_ = true por padrão (fundo preto/linhas
// brancas, igual ao visual original deste aparelho), trocável em tempo real
// via SetTheme() - ver ApplyFaceColors().
lv_color_t RobotFaceDisplay::FaceBgColor() const {
    return dark_theme_ ? lv_color_black() : lv_color_make(0xE8, 0xE8, 0xE8);
}
lv_color_t RobotFaceDisplay::FaceFgColor() const {
    return dark_theme_ ? lv_color_white() : lv_color_black();
}

RobotFaceDisplay::RobotFaceDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                    int width, int height, int offset_x, int offset_y,
                                    bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
}

RobotFaceDisplay::~RobotFaceDisplay() {
    if (blink_timer_) lv_timer_del(blink_timer_);
    if (blink_end_timer_) lv_timer_del(blink_end_timer_);
    if (gaze_timer_) lv_timer_del(gaze_timer_);
    if (gaze_end_timer_) lv_timer_del(gaze_end_timer_);
}

void RobotFaceDisplay::SetAudioCodecForSync(AudioCodec* codec) {
    audio_codec_ = codec;
    if (audio_codec_ != nullptr) {
        audio_codec_->SetAudioLevelCallback([this](float level) {
            // Executa na task de saída de áudio - precisa do lock do LVGL.
            OnAudioLevel(level);
        });
    }
}

void RobotFaceDisplay::SetupUI() {
    // Só a UI normal (status/texto) é construída no boot. Os widgets do rosto
    // são criados sob demanda na primeira vez que o modo rosto é ativado, para
    // não competir por RAM com o pico de TLS do OTA/MQTT durante a inicialização.
    SpiLcdDisplay::SetupUI();
}

void RobotFaceDisplay::CreateFaceWidgets() {
    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();

    face_container_ = lv_obj_create(screen);
    lv_obj_set_size(face_container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(face_container_, 0, 0);
    lv_obj_set_style_pad_all(face_container_, 0, 0);
    lv_obj_set_style_border_width(face_container_, 0, 0);
    lv_obj_set_style_bg_color(face_container_, FaceBgColor(), 0);
    lv_obj_set_style_bg_opa(face_container_, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(face_container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(face_container_, LV_ALIGN_CENTER, 0, 0);

    // --- Olhos estilo "sólido": preenchidos, com um brilho (glint) na cor
    // do fundo por cima, em vez do anel+pupila antigo.
    auto make_eye = [&](int x_off) {
        lv_obj_t* eye = lv_obj_create(face_container_);
        lv_obj_set_size(eye, kEyeW, kEyeH);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(eye, FaceFgColor(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_scrollbar_mode(eye, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(eye, LV_ALIGN_CENTER, x_off, kEyeCy - kCy);
        return eye;
    };
    left_eye_ = make_eye(kEyeCxL - kCx);
    right_eye_ = make_eye(kEyeCxR - kCx);

    auto make_glint = [&](int x_off) {
        lv_obj_t* glint = lv_obj_create(face_container_);
        lv_obj_set_size(glint, kPupilR * 2, kPupilR * 2);
        lv_obj_set_style_radius(glint, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(glint, FaceBgColor(), 0);
        lv_obj_set_style_bg_opa(glint, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(glint, 0, 0);
        lv_obj_set_scrollbar_mode(glint, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(glint, LV_ALIGN_CENTER, x_off, kEyeCy - kCy);
        lv_obj_add_flag(glint, LV_OBJ_FLAG_HIDDEN);
        return glint;
    };
    left_pupil_ = make_glint(kEyeCxL - kCx);
    right_pupil_ = make_glint(kEyeCxR - kCx);

    // --- Olho fechado: linha RETA (diferente da sobrancelha arqueada, senão
    // parece que o robô tem 4 sobrancelhas) - usada pra dormir, piscar e
    // expressões com olhos semicerrados/fechados.
    auto make_closed_eye = [&](lv_point_precise_t* pts) {
        lv_obj_t* line = lv_line_create(face_container_);
        lv_obj_set_size(line, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_set_style_line_width(line, 7, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_color(line, FaceFgColor(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_line_set_points(line, pts, 2);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        return line;
    };
    eye_closed_l_pts_[0] = {(lv_value_precise_t)(kEyeCxL - 26), (lv_value_precise_t)kEyeCy};
    eye_closed_l_pts_[1] = {(lv_value_precise_t)(kEyeCxL + 26), (lv_value_precise_t)kEyeCy};
    eye_closed_r_pts_[0] = {(lv_value_precise_t)(kEyeCxR - 26), (lv_value_precise_t)kEyeCy};
    eye_closed_r_pts_[1] = {(lv_value_precise_t)(kEyeCxR + 26), (lv_value_precise_t)kEyeCy};
    left_eye_closed_ = make_closed_eye(eye_closed_l_pts_);
    right_eye_closed_ = make_closed_eye(eye_closed_r_pts_);

    // --- Sobrancelhas e boca: linhas, cobrindo a tela toda com coordenadas
    // absolutas (iguais às do mockup), assim não precisamos converter para
    // offsets relativos ao desenhar cada expressão.
    auto make_line = [&](lv_point_precise_t* pts, int n) {
        lv_obj_t* line = lv_line_create(face_container_);
        lv_obj_set_size(line, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_set_style_line_width(line, kBrowW, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_color(line, FaceFgColor(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_line_set_points(line, pts, n);
        return line;
    };
    left_brow_ = make_line(brow_l_pts_, 3);
    right_brow_ = make_line(brow_r_pts_, 3);
    mouth_ = make_line(mouth_pts_, kMouthPts);
    lv_obj_set_style_line_width(mouth_, kMouthLineW, 0);
    // Boca "círculo" de repouso (Surprised/Shocked/etc.) reaproveita esta
    // mesma linha - RenderMouth() já amostra uma elipse fechada nela, sem
    // precisar de um widget de borda separado.

    // --- Boca "com volume" usada enquanto fala: uma forma preenchida (não
    // um traço) que cresce/encolhe em ALTURA por estados discretos, como
    // uma boca de desenho animado abrindo de verdade - ver SetMouthState().
    mouth_open_shape_ = lv_obj_create(face_container_);
    lv_obj_set_style_radius(mouth_open_shape_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mouth_open_shape_, FaceFgColor(), 0);
    lv_obj_set_style_bg_opa(mouth_open_shape_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mouth_open_shape_, 0, 0);
    lv_obj_set_scrollbar_mode(mouth_open_shape_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(mouth_open_shape_, LV_OBJ_FLAG_HIDDEN);

    // Este aparelho (ESP32-S3, com PSRAM) tem folga de RAM - mantém a linha
    // de "dentes" separada e a língua como widget próprio (mouth_tongue_accent_),
    // ao contrário do C3 que reaproveita `tongue_` pra economizar RAM.
    mouth_teeth_line_ = lv_obj_create(face_container_);
    lv_obj_set_style_radius(mouth_teeth_line_, 2, 0);
    lv_obj_set_style_bg_color(mouth_teeth_line_, FaceBgColor(), 0);
    lv_obj_set_style_bg_opa(mouth_teeth_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mouth_teeth_line_, 0, 0);
    lv_obj_set_scrollbar_mode(mouth_teeth_line_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(mouth_teeth_line_, LV_OBJ_FLAG_HIDDEN);

    mouth_tongue_accent_ = lv_obj_create(face_container_);
    lv_obj_set_style_radius(mouth_tongue_accent_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mouth_tongue_accent_, lv_color_make(0xFF, 0x6F, 0xA8), 0);
    lv_obj_set_style_bg_opa(mouth_tongue_accent_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mouth_tongue_accent_, 0, 0);
    lv_obj_set_scrollbar_mode(mouth_tongue_accent_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(mouth_tongue_accent_, LV_OBJ_FLAG_HIDDEN);

    // --- Decorações (escondidas por padrão; cada expressão liga a sua). ---
    auto make_dot = [&](int d, lv_color_t color) {
        lv_obj_t* o = lv_obj_create(face_container_);
        lv_obj_set_size(o, d, d);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(o, color, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    };
    lv_color_t pink = lv_color_make(0xFF, 0x6F, 0xA8);
    lv_color_t blue = lv_color_make(0x6F, 0xC6, 0xFF);

    tear_l_ = make_dot(14, blue);
    tear_r_ = make_dot(14, blue);
    blush_l_ = make_dot(18, pink);
    blush_r_ = make_dot(18, pink);
    tongue_ = make_dot(20, pink);

    // Óculos (Cool): 2 lentes arredondadas + ponte + hastes, em vez de uma
    // barra única.
    auto make_lens = [&](int x_off) {
        lv_obj_t* l = lv_obj_create(face_container_);
        lv_obj_set_size(l, 62, 38);
        lv_obj_set_style_radius(l, 10, 0);
        lv_obj_set_style_bg_color(l, FaceFgColor(), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(l, 0, 0);
        lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(l, LV_ALIGN_CENTER, x_off, 104 - kCy);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        return l;
    };
    lens_l_ = make_lens(kEyeCxL - kCx);
    lens_r_ = make_lens(kEyeCxR - kCx);
    glasses_bridge_pts_[0] = {129, 98};
    glasses_bridge_pts_[1] = {167, 98};
    glasses_arm_l_pts_[0] = {40, 94};
    glasses_arm_l_pts_[1] = {67, 100};
    glasses_arm_r_pts_[0] = {229, 100};
    glasses_arm_r_pts_[1] = {256, 94};
    auto make_glasses_line = [&](lv_point_precise_t* pts) {
        lv_obj_t* line = lv_line_create(face_container_);
        lv_obj_set_size(line, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_set_style_line_width(line, 6, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_color(line, FaceFgColor(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_line_set_points(line, pts, 2);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        return line;
    };
    glasses_bridge_ = make_glasses_line(glasses_bridge_pts_);
    glasses_arm_l_ = make_glasses_line(glasses_arm_l_pts_);
    glasses_arm_r_ = make_glasses_line(glasses_arm_r_pts_);

    zzz_label_ = lv_label_create(face_container_);
    lv_label_set_text(zzz_label_, "zZz");
    lv_obj_set_style_text_color(zzz_label_, FaceFgColor(), 0);
    lv_obj_align(zzz_label_, LV_ALIGN_CENTER, kZzzOffsetX, kZzzOffsetY);

    face_widgets_created_ = true;
    // Aplica a expressão neutra "acordada" agora - a tela ainda está
    // escondida (face_container_ só aparece depois, em EnterFaceMode), então
    // isso não pisca. O SetSleeping(true) que roda logo em seguida (chamado
    // por UpdateFaceModeFromState ao entrar no modo rosto pela 1a vez, que é
    // sempre a partir de Idle) fecha os olhos de verdade. sleeping_ começa em
    // false para essa chamada não ser ignorada por "já estar aplicada".
    ApplyExpression(RobotExpression::kNeutral);
}

void RobotFaceDisplay::EnterFaceMode() {
    if (!face_widgets_created_) {
        CreateFaceWidgets();
    }
    face_mode_ = true;
    {
        DisplayLockGuard lock(this);
        lv_obj_move_foreground(face_container_);
        lv_obj_remove_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
    }
    ScheduleNextBlink();
    ScheduleNextGaze();
}

void RobotFaceDisplay::ExitFaceMode() {
    face_mode_ = false;
    mouth_level_smoothed_ = 0.0f;
    {
        DisplayLockGuard lock(this);
        if (blink_timer_) { lv_timer_del(blink_timer_); blink_timer_ = nullptr; }
        if (blink_end_timer_) { lv_timer_del(blink_end_timer_); blink_end_timer_ = nullptr; }
        if (gaze_timer_) { lv_timer_del(gaze_timer_); gaze_timer_ = nullptr; }
        if (gaze_end_timer_) { lv_timer_del(gaze_end_timer_); gaze_end_timer_ = nullptr; }
        if (face_container_ != nullptr) {
            lv_obj_add_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Reaplica tamanho dos olhos (normal ou "atento") e posição das pupilas
// (centro + deslocamento do olhar). Só usado para o par de olhos "padrão";
// expressões com formato especial (Loving/Cool/Winking/etc.) são tratadas
// em ApplyExpression e restauradas via ApplyExpression() após piscar.
void RobotFaceDisplay::ApplyEyeGeometry() {
    DisplayLockGuard lock(this);
    if (!eyes_open_) {
        return;
    }
    int w = attentive_ ? kEyeWAttentive : kEyeW;
    int h = attentive_ ? kEyeHAttentive : kEyeH;
    lv_obj_set_size(left_eye_, w, h);
    lv_obj_set_size(right_eye_, w, h);
    lv_obj_align(left_eye_, LV_ALIGN_CENTER, kEyeCxL - kCx, kEyeCy - kCy);
    lv_obj_align(right_eye_, LV_ALIGN_CENTER, kEyeCxR - kCx, kEyeCy - kCy);
    // Brilho (glint) centralizado no olho, deslocado pelo "olhar ao redor"
    // (gaze_dx_/gaze_dy_).
    lv_obj_align(left_pupil_, LV_ALIGN_CENTER, kEyeCxL - kCx + gaze_dx_, kEyeCy - kCy + gaze_dy_);
    lv_obj_align(right_pupil_, LV_ALIGN_CENTER, kEyeCxR - kCx + gaze_dx_, kEyeCy - kCy + gaze_dy_);
}

void RobotFaceDisplay::SetEyesOpen(bool open) {
    eyes_open_ = open;
    if (open) {
        {
            DisplayLockGuard lock(this);
            lv_obj_add_flag(left_eye_closed_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(right_eye_closed_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(left_pupil_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_pupil_, LV_OBJ_FLAG_HIDDEN);
        }
        gaze_dx_ = 0;
        gaze_dy_ = 0;
        ApplyEyeGeometry();
    } else {
        DisplayLockGuard lock(this);
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_pupil_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_pupil_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(left_eye_closed_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(right_eye_closed_, LV_OBJ_FLAG_HIDDEN);
    }
}

void RobotFaceDisplay::SetAttentive(bool attentive) {
    if (attentive_ == attentive) return;
    attentive_ = attentive;
    if (eyes_open_ && expression_ == RobotExpression::kNeutral) {
        ApplyEyeGeometry();
    }
}

void RobotFaceDisplay::ScheduleNextGaze() {
    if (gaze_timer_) lv_timer_del(gaze_timer_);
    uint32_t period = 1500 + (esp_random() % 2000);
    gaze_timer_ = lv_timer_create(GazeTimerCb, period, this);
    lv_timer_set_repeat_count(gaze_timer_, 1);
}

void RobotFaceDisplay::GazeTimerCb(lv_timer_t* timer) {
    auto self = static_cast<RobotFaceDisplay*>(lv_timer_get_user_data(timer));
    self->gaze_timer_ = nullptr;
    if (self->face_mode_ && !self->sleeping_ && self->eyes_open_ &&
        self->expression_ == RobotExpression::kNeutral) {
        self->gaze_dx_ = (int)(esp_random() % (2 * kGazeMaxDx + 1)) - kGazeMaxDx;
        self->gaze_dy_ = (int)(esp_random() % (2 * kGazeMaxDy + 1)) - kGazeMaxDy;
        self->ApplyEyeGeometry();
        if (self->gaze_end_timer_) lv_timer_del(self->gaze_end_timer_);
        self->gaze_end_timer_ = lv_timer_create(GazeEndTimerCb, 400 + (esp_random() % 400), self);
        lv_timer_set_repeat_count(self->gaze_end_timer_, 1);
    } else if (self->face_mode_ && !self->sleeping_) {
        self->ScheduleNextGaze();
    }
}

void RobotFaceDisplay::GazeEndTimerCb(lv_timer_t* timer) {
    auto self = static_cast<RobotFaceDisplay*>(lv_timer_get_user_data(timer));
    self->gaze_end_timer_ = nullptr;
    self->gaze_dx_ = 0;
    self->gaze_dy_ = 0;
    if (self->face_mode_ && !self->sleeping_ && self->eyes_open_ &&
        self->expression_ == RobotExpression::kNeutral) {
        self->ApplyEyeGeometry();
    }
    if (self->face_mode_ && !self->sleeping_) {
        self->ScheduleNextGaze();
    }
}

void RobotFaceDisplay::SetSleeping(bool sleeping) {
    if (sleeping == sleeping_ && face_mode_) return;
    sleeping_ = sleeping;
    if (sleeping_) {
        SetEyesOpen(false);
        DisplayLockGuard lock(this);
        // A boca (linha de repouso) fica visível durante o sono - só a
        // versão "falando" (forma preenchida/dentes/língua) fica escondida.
        lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_open_shape_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_teeth_line_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_tongue_accent_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tear_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tear_r_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blush_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blush_r_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tongue_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lens_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lens_r_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(glasses_bridge_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(glasses_arm_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(glasses_arm_r_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(zzz_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        {
            DisplayLockGuard lock(this);
            lv_obj_add_flag(zzz_label_, LV_OBJ_FLAG_HIDDEN);
        }
        // Restaura a expressão atual por completo (olhos, sobrancelha, boca,
        // decorações) em vez de só abrir os olhos no formato padrão.
        ApplyExpression(expression_);
        ScheduleNextBlink();
        ScheduleNextGaze();
    }
}

void RobotFaceDisplay::ScheduleNextBlink() {
    if (blink_timer_) lv_timer_del(blink_timer_);
    uint32_t period = 3000 + (esp_random() % 3000);
    blink_timer_ = lv_timer_create(BlinkTimerCb, period, this);
    lv_timer_set_repeat_count(blink_timer_, 1);
}

void RobotFaceDisplay::BlinkTimerCb(lv_timer_t* timer) {
    auto self = static_cast<RobotFaceDisplay*>(lv_timer_get_user_data(timer));
    self->blink_timer_ = nullptr;
    if (self->face_mode_ && !self->sleeping_) {
        self->SetEyesOpen(false);
        if (self->blink_end_timer_) lv_timer_del(self->blink_end_timer_);
        self->blink_end_timer_ = lv_timer_create(BlinkEndTimerCb, 130, self);
        lv_timer_set_repeat_count(self->blink_end_timer_, 1);
    }
}

void RobotFaceDisplay::BlinkEndTimerCb(lv_timer_t* timer) {
    auto self = static_cast<RobotFaceDisplay*>(lv_timer_get_user_data(timer));
    self->blink_end_timer_ = nullptr;
    if (self->face_mode_ && !self->sleeping_) {
        // Restaura a expressão atual (não só "olho aberto genérico"), para
        // preservar formatos assimétricos como Winking/Funny/Silly.
        self->ApplyExpression(self->expression_);
        self->ScheduleNextBlink();
    }
}

// ---------------------------------------------------------------------------
// Boca: desenhada como linha (curva ou "círculo" fechado), com um parâmetro
// de "abertura" que a fala anima continuamente por cima do formato de
// repouso da expressão atual - assim a boca se mexe em QUALQUER expressão.
// ---------------------------------------------------------------------------
void RobotFaceDisplay::RenderMouth(float openness) {
    DisplayLockGuard lock(this);
    if (mouth_is_circle_) {
        float r = mouth_cr_ + openness;
        if (r < 4) r = 4;
        for (int i = 0; i < kMouthPts; i++) {
            float ang = (2.0f * 3.14159265f * i) / (kMouthPts - 1);
            mouth_pts_[i].x = mouth_ccx_ + r * cosf(ang);
            mouth_pts_[i].y = mouth_ccy_ + r * 0.75f * sinf(ang);
        }
    } else {
        float cy = mouth_cy_ + openness;
        int half = kMouthPts / 2;
        for (int i = 0; i < kMouthPts; i++) {
            float t = (float)i / (kMouthPts - 1);
            float x = mouth_x0_ + (mouth_x1_ - mouth_x0_) * t;
            // Bézier quadrática simples entre (x0,y0)-(cx,cy)-(x1,y1).
            float y = (1 - t) * (1 - t) * mouth_y0_ + 2 * (1 - t) * t * cy + t * t * mouth_y1_;
            mouth_pts_[i].x = x;
            mouth_pts_[i].y = y;
        }
        (void)half;
    }
    lv_line_set_points(mouth_, mouth_pts_, kMouthPts);
}

// Boca "com volume" enquanto fala: troca entre 4 formas preenchidas
// (fechada/pequena/média/aberta), como uma boca de desenho animado -
// diferente de deformar uma curva continuamente, que só "vibra" porque um
// traço sem preenchimento não comunica abertura de jeito nenhum.
void RobotFaceDisplay::SetMouthState(int state) {
    DisplayLockGuard lock(this);
    mouth_state_ = state;

    if (state <= 0) {
        lv_obj_add_flag(mouth_open_shape_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_teeth_line_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_tongue_accent_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);

    static const int kHeights[] = {0, 16, 28, 46};
    int h = kHeights[state > 3 ? 3 : state];

    float cx, cy, w;
    if (mouth_is_circle_) {
        cx = mouth_ccx_;
        cy = mouth_ccy_;
        w = mouth_cr_ * 1.9f;
    } else {
        cx = (mouth_x0_ + mouth_x1_) / 2.0f;
        cy = (mouth_y0_ + mouth_y1_) / 2.0f;
        w = fabsf(mouth_x1_ - mouth_x0_) * 1.05f;
        if (w < 28) w = 28;
    }

    lv_obj_set_size(mouth_open_shape_, (int)w, h);
    lv_obj_align(mouth_open_shape_, LV_ALIGN_CENTER, (int)cx - kCx, (int)cy - kCy);
    lv_obj_remove_flag(mouth_open_shape_, LV_OBJ_FLAG_HIDDEN);

    if (state >= 2) {
        // Estados médio/aberto: uma linha (na cor do fundo) perto do topo
        // sugere a borda dos "dentes" (como nas referências de boca de
        // desenho animado), e uma língua rosa preenche a parte de baixo da
        // cavidade.
        int teeth_w = (int)(w * 0.8f);
        int teeth_y = h / 2 - (h >= 20 ? 6 : 3);
        lv_obj_set_size(mouth_teeth_line_, teeth_w, 3);
        lv_obj_align(mouth_teeth_line_, LV_ALIGN_CENTER, (int)cx - kCx, (int)cy - kCy - teeth_y);
        lv_obj_remove_flag(mouth_teeth_line_, LV_OBJ_FLAG_HIDDEN);

        int tongue_w = (int)(w * (state >= 3 ? 0.55f : 0.4f));
        int tongue_h = h / 2;
        if (tongue_w < 10) tongue_w = 10;
        if (tongue_h < 5) tongue_h = 5;
        lv_obj_set_size(mouth_tongue_accent_, tongue_w, tongue_h);
        lv_obj_align(mouth_tongue_accent_, LV_ALIGN_CENTER,
                     (int)cx - kCx, (int)cy - kCy + h / 2 - tongue_h / 2 + 1);
        lv_obj_remove_flag(mouth_tongue_accent_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(mouth_teeth_line_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_tongue_accent_, LV_OBJ_FLAG_HIDDEN);
    }
}

// Chamado (na task de saída de áudio) a cada bloco de PCM realmente enviado
// ao alto-falante, com a amplitude média do bloco (0.0-1.0). Abre rápido
// (ataque) e fecha mais devagar (release), como um VU-meter, para suavizar
// o nível bruto antes de decidir o estado discreto da boca.
void RobotFaceDisplay::OnAudioLevel(float level) {
    if (level > mouth_level_smoothed_) {
        mouth_level_smoothed_ = mouth_level_smoothed_ * 0.35f + level * 0.65f;
    } else {
        mouth_level_smoothed_ = mouth_level_smoothed_ * 0.8f + level * 0.2f;
    }
    if (!face_mode_ || sleeping_) {
        return;
    }
    // Limita a taxa de verificação (a task de áudio pode chamar isto a cada
    // 20-30ms) para não sobrecarregar o LVGL.
    int64_t now = esp_timer_get_time();
    if (now - last_mouth_render_us_ < 60000) {
        return;
    }
    last_mouth_render_us_ = now;

    int target_state;
    if (mouth_level_smoothed_ < 0.06f) target_state = 0;
    else if (mouth_level_smoothed_ < 0.18f) target_state = 1;
    else if (mouth_level_smoothed_ < 0.40f) target_state = 2;
    else target_state = 3;

    // Só troca de estado se já passou um tempo mínimo no estado atual -
    // imita "animação em twos/threes" (poses seguram por vários frames) e
    // evita que ruído do áudio faça a boca "vibrar" entre estados a cada
    // poucos milissegundos.
    const int64_t kMinHoldUs = 90000;
    if (target_state != mouth_state_ && (now - mouth_state_since_us_) >= kMinHoldUs) {
        mouth_state_since_us_ = now;
        SetMouthState(target_state);
    }
}

// ---------------------------------------------------------------------------
// Expressões: mapeia as ~21 emoções que o app manda via SetEmotion() para um
// formato de olhos/sobrancelha/boca. Cada expressão define tudo do zero
// (o que não é usado fica escondido), então não há estado residual entre trocas.
// ---------------------------------------------------------------------------
namespace {
struct P { float x, y; };
}

void RobotFaceDisplay::ApplyExpression(RobotExpression expr) {
    expression_ = expr;
    DisplayLockGuard lock(this);

    // Esconde tudo que é "extra" por padrão; cada case liga o que precisa.
    lv_obj_add_flag(tear_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tear_r_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(blush_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(blush_r_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tongue_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lens_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lens_r_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(glasses_bridge_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(glasses_arm_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(glasses_arm_r_, LV_OBJ_FLAG_HIDDEN);

    if (sleeping_) {
        // A cara "dormindo" (zZz + olhos fechados) já está montada por
        // SetSleeping(); aqui só guardamos qual expressão fica pendente
        // para quando acordar, sem tocar na aparência dos olhos.
        return;
    }
    lv_obj_set_style_opa(left_pupil_, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(right_pupil_, LV_OPA_COVER, 0);
    lv_obj_add_flag(left_eye_closed_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_closed_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);

    auto brow = [&](lv_point_precise_t* pts, P a, P c, P b) {
        pts[0].x = (lv_value_precise_t)a.x; pts[0].y = (lv_value_precise_t)a.y;
        pts[1].x = (lv_value_precise_t)c.x; pts[1].y = (lv_value_precise_t)c.y;
        pts[2].x = (lv_value_precise_t)b.x; pts[2].y = (lv_value_precise_t)b.y;
    };
    auto closed_line_for = [&](lv_obj_t* eye) {
        return (eye == left_eye_) ? left_eye_closed_ : right_eye_closed_;
    };
    auto closed_pts_for = [&](lv_obj_t* eye) {
        return (eye == left_eye_) ? eye_closed_l_pts_ : eye_closed_r_pts_;
    };
    // Olho "sólido": preenchido + brilho (glint) na cor do fundo, centralizado
    // no olho. hide=true esconde o olho INTEIRO (usado em Cool). color, se
    // passado, sobrepõe a cor normal do olho (usado em Loving, pra pintar
    // de rosa).
    auto eye_open = [&](lv_obj_t* eye, lv_obj_t* pupil, int cx, int cy, int w, int h, int pr,
                         bool hide = false, const lv_color_t* color = nullptr) {
        lv_obj_add_flag(closed_line_for(eye), LV_OBJ_FLAG_HIDDEN);
        if (hide) {
            lv_obj_add_flag(eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pupil, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_remove_flag(eye, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(eye, w, h);
        lv_obj_align(eye, LV_ALIGN_CENTER, cx - kCx, cy - kCy);
        lv_obj_set_style_bg_color(eye, color != nullptr ? *color : FaceFgColor(), 0);
        lv_obj_set_size(pupil, pr * 2, pr * 2);
        lv_obj_align(pupil, LV_ALIGN_CENTER, cx - kCx, cy - kCy);
        lv_obj_set_style_opa(pupil, LV_OPA_COVER, 0);  // reseta o efeito do Sleepy2
        lv_obj_remove_flag(pupil, LV_OBJ_FLAG_HIDDEN);
    };
    // Olho fechado: linha RETA (a sobrancelha já é arqueada - se o olho
    // fechado também curvar, parece que o robô tem 4 sobrancelhas).
    auto eye_closed = [&](lv_obj_t* eye, lv_obj_t* pupil, int cx, int cy) {
        lv_obj_add_flag(eye, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pupil, LV_OBJ_FLAG_HIDDEN);
        lv_point_precise_t* pts = closed_pts_for(eye);
        pts[0].x = (lv_value_precise_t)(cx - 26); pts[0].y = (lv_value_precise_t)cy;
        pts[1].x = (lv_value_precise_t)(cx + 26); pts[1].y = (lv_value_precise_t)cy;
        lv_obj_t* line = closed_line_for(eye);
        lv_line_set_points(line, pts, 2);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
    };
    auto mouth_curve = [&](float x0, float y0, float cx, float cy, float x1, float y1) {
        mouth_is_circle_ = false;
        mouth_x0_ = x0; mouth_y0_ = y0; mouth_cx_ = cx; mouth_cy_ = cy; mouth_x1_ = x1; mouth_y1_ = y1;
    };
    auto mouth_circle = [&](float ccx, float ccy, float r) {
        mouth_is_circle_ = true;
        mouth_ccx_ = ccx; mouth_ccy_ = ccy; mouth_cr_ = r;
    };

    switch (expr) {
    case RobotExpression::kNeutral:
        brow(brow_l_pts_, {78,66}, {98,60}, {118,66});
        brow(brow_r_pts_, {178,66}, {198,60}, {218,66});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 112, 64, 64, 9);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 112, 64, 64, 9);
        mouth_curve(130,182, 148,190, 166,182);
        break;

    case RobotExpression::kHappy:
        brow(brow_l_pts_, {76,58}, {98,48}, {120,58});
        brow(brow_r_pts_, {176,58}, {198,48}, {220,58});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 110, 64, 56, 9);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 110, 64, 56, 9);
        mouth_curve(114,176, 148,204, 182,176);
        break;

    case RobotExpression::kLaughing:
        brow(brow_l_pts_, {74,52}, {98,40}, {122,52});
        brow(brow_r_pts_, {174,52}, {198,40}, {222,52});
        eye_closed(left_eye_, left_pupil_, kEyeCxL, 108);
        eye_closed(right_eye_, right_pupil_, kEyeCxR, 108);
        mouth_curve(108,172, 148,214, 188,172);
        break;

    case RobotExpression::kFunny:
        brow(brow_l_pts_, {72,50}, {94,40}, {118,52});
        brow(brow_r_pts_, {178,74}, {198,72}, {218,76});
        eye_open(left_eye_, left_pupil_, 96, 104, 70, 72, 10);
        eye_open(right_eye_, right_pupil_, 200, 116, 54, 54, 8);
        mouth_curve(118,178, 152,200, 186,170);
        break;

    case RobotExpression::kSad:
        brow(brow_l_pts_, {74,78}, {98,72}, {122,60});
        brow(brow_r_pts_, {174,60}, {198,72}, {222,78});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 118, 58, 56, 8);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 118, 58, 56, 8);
        mouth_curve(122,190, 148,172, 174,190);
        break;

    case RobotExpression::kAngry:
        brow(brow_l_pts_, {72,56}, {98,68}, {126,84});
        brow(brow_r_pts_, {170,84}, {198,68}, {224,56});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 118, 62, 40, 7);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 118, 62, 40, 7);
        mouth_curve(124,192, 148,174, 172,192);
        break;

    case RobotExpression::kCrying:
        brow(brow_l_pts_, {74,82}, {98,74}, {122,60});
        brow(brow_r_pts_, {174,60}, {198,74}, {222,82});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 120, 52, 48, 7);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 120, 52, 48, 7);
        mouth_curve(120,194, 148,174, 176,194);
        lv_obj_remove_flag(tear_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(tear_r_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(tear_l_, LV_ALIGN_CENTER, 80 - kCx, 158 - kCy);
        lv_obj_align(tear_r_, LV_ALIGN_CENTER, 216 - kCx, 158 - kCy);
        break;

    case RobotExpression::kLoving: {
        // Sem coração (widget rotacionado travava o C3) - olhos com o
        // formato normal, só que na cor rosa.
        lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_color_t loving_pink = lv_color_make(0xFF, 0x4D, 0x6D);
        eye_open(left_eye_, left_pupil_, kEyeCxL, 112, 64, 64, 9, false, &loving_pink);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 112, 64, 64, 9, false, &loving_pink);
        mouth_curve(116,178, 148,204, 180,178);
        break;
    }

    case RobotExpression::kEmbarrassed:
        brow(brow_l_pts_, {80,84}, {98,81}, {116,84});
        brow(brow_r_pts_, {180,84}, {198,81}, {216,84});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 118, 52, 52, 7);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 118, 52, 52, 7);
        mouth_curve(134,186, 148,180, 162,186);
        lv_obj_remove_flag(blush_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(blush_r_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(blush_l_, LV_ALIGN_CENTER, 66 - kCx, 140 - kCy);
        lv_obj_align(blush_r_, LV_ALIGN_CENTER, 230 - kCx, 140 - kCy);
        break;

    case RobotExpression::kSurprised:
        brow(brow_l_pts_, {72,44}, {98,32}, {124,44});
        brow(brow_r_pts_, {172,44}, {198,32}, {224,44});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 106, 76, 80, 7);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 106, 76, 80, 7);
        mouth_circle(148, 186, 13);
        break;

    case RobotExpression::kShocked:
        brow(brow_l_pts_, {68,34}, {98,26}, {128,34});
        brow(brow_r_pts_, {168,34}, {198,26}, {228,34});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 102, 86, 92, 5);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 102, 86, 92, 5);
        mouth_circle(148, 192, 18);
        break;

    case RobotExpression::kThinking:
        brow(brow_l_pts_, {72,60}, {96,48}, {122,58});
        brow(brow_r_pts_, {178,70}, {198,68}, {218,72});
        eye_open(left_eye_, left_pupil_, 90, 104, 64, 64, 9);
        eye_open(right_eye_, right_pupil_, 190, 112, 64, 64, 9);
        mouth_curve(132,182, 150,177, 166,183);
        break;

    case RobotExpression::kWinking:
        brow(brow_l_pts_, {76,82}, {98,76}, {120,82});
        brow(brow_r_pts_, {176,60}, {198,50}, {220,60});
        eye_closed(left_eye_, left_pupil_, kEyeCxL, 112);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 110, 64, 64, 9);
        mouth_curve(124,178, 148,196, 180,174);
        break;

    case RobotExpression::kCool:
        brow(brow_l_pts_, {72,58}, {98,54}, {124,58});
        brow(brow_r_pts_, {172,58}, {198,54}, {224,58});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 112, 64, 64, 9, true);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 112, 64, 64, 9, true);
        mouth_curve(126,180, 150,170, 174,182);
        lv_obj_remove_flag(lens_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lens_r_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(glasses_bridge_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(glasses_arm_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(glasses_arm_r_, LV_OBJ_FLAG_HIDDEN);
        break;

    case RobotExpression::kRelaxed:
        brow(brow_l_pts_, {78,78}, {98,70}, {118,78});
        brow(brow_r_pts_, {178,78}, {198,70}, {218,78});
        eye_closed(left_eye_, left_pupil_, kEyeCxL, 114);
        eye_closed(right_eye_, right_pupil_, kEyeCxR, 114);
        mouth_curve(128,180, 148,192, 168,180);
        break;

    case RobotExpression::kDelicious:
        brow(brow_l_pts_, {76,62}, {98,52}, {120,62});
        brow(brow_r_pts_, {176,62}, {198,52}, {220,62});
        eye_closed(left_eye_, left_pupil_, kEyeCxL, 112);
        eye_closed(right_eye_, right_pupil_, kEyeCxR, 112);
        mouth_circle(148, 188, 16);
        break;

    case RobotExpression::kKiss:
        brow(brow_l_pts_, {78,70}, {98,62}, {118,70});
        brow(brow_r_pts_, {178,70}, {198,62}, {218,70});
        eye_closed(left_eye_, left_pupil_, kEyeCxL, 116);
        eye_closed(right_eye_, right_pupil_, kEyeCxR, 116);
        mouth_circle(148, 186, 8);
        break;

    case RobotExpression::kConfident:
        brow(brow_l_pts_, {74,60}, {98,54}, {122,62});
        brow(brow_r_pts_, {174,64}, {198,60}, {222,70});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 114, 62, 54, 9);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 114, 62, 54, 9);
        mouth_curve(126,182, 148,168, 174,184);
        break;

    case RobotExpression::kSleepy2:
        brow(brow_l_pts_, {74,84}, {98,84}, {122,86});
        brow(brow_r_pts_, {174,86}, {198,84}, {222,84});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 120, 64, 34, 7);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 120, 64, 34, 7);
        // brilho a 60% (olhar apagado de sono)
        lv_obj_set_style_opa(left_pupil_, LV_OPA_60, 0);
        lv_obj_set_style_opa(right_pupil_, LV_OPA_60, 0);
        mouth_curve(132,186, 148,190, 164,186);
        break;

    case RobotExpression::kSilly:
        brow(brow_l_pts_, {70,46}, {96,36}, {124,50});
        brow(brow_r_pts_, {178,80}, {198,86}, {218,82});
        eye_open(left_eye_, left_pupil_, 96, 104, 72, 76, 10);
        eye_open(right_eye_, right_pupil_, 200, 120, 46, 46, 6);
        mouth_curve(112,178, 148,206, 192,168);
        lv_obj_remove_flag(tongue_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(tongue_, LV_ALIGN_CENTER, 150 - kCx, 190 - kCy);
        break;

    case RobotExpression::kConfused:
        brow(brow_l_pts_, {70,46}, {98,36}, {126,50});
        brow(brow_r_pts_, {176,76}, {198,80}, {220,76});
        eye_open(left_eye_, left_pupil_, kEyeCxL, 106, 64, 64, 9);
        eye_open(right_eye_, right_pupil_, kEyeCxR, 116, 54, 54, 8);
        // brilhos deslocados pra dentro (olhar perdido)
        lv_obj_set_x(left_pupil_, lv_obj_get_x(left_pupil_) + 10);
        lv_obj_set_x(right_pupil_, lv_obj_get_x(right_pupil_) - 10);
        mouth_curve(128,186, 148,178, 166,188);
        break;
    }

    // Reaplica o estado atual da boca (fechada ou a forma de "falando" em
    // andamento) na nova expressão, já que os pontos de referência da boca
    // (mouth_x0_/x1_/cx_/cy_ ou mouth_ccx_/ccy_/cr_) acabaram de mudar acima.
    RenderMouth(0);
    SetMouthState(speaking_ ? mouth_state_ : 0);
}

void RobotFaceDisplay::UpdateFaceModeFromState() {
    DeviceState state = Application::GetInstance().GetDeviceState();
    // kDeviceStateConnecting aqui é só a reabertura do canal de áudio com o
    // servidor (ex: ao apertar o botão de escuta) - não é desconexão de Wi-Fi.
    // Mantemos o rosto ativo (já "acordado") para não piscar a UI antiga.
    bool want_face = (state == kDeviceStateIdle ||
                       state == kDeviceStateConnecting ||
                       state == kDeviceStateListening ||
                       state == kDeviceStateSpeaking);

    if (want_face && !face_mode_) {
        EnterFaceMode();
    } else if (!want_face && face_mode_) {
        ExitFaceMode();
    }

    if (face_mode_) {
        SetSleeping(state == kDeviceStateIdle);
        SetAttentive(state == kDeviceStateListening);
        bool was_speaking = speaking_;
        speaking_ = (state == kDeviceStateSpeaking);
        if (was_speaking && !speaking_) {
            // Saiu de "falando": fecha a boca. Não precisa de folga/timer -
            // a boca só se mexe quando Write() manda áudio de verdade, então
            // uma pausa breve entre frases simplesmente não gera chamadas
            // (a boca fica parada, não "trava" no meio de um movimento).
            mouth_level_smoothed_ = 0.0f;
            mouth_state_since_us_ = 0;
            SetMouthState(0);
        }
    }
}

void RobotFaceDisplay::SetStatus(const char* status) {
    UpdateFaceModeFromState();
    if (face_mode_) {
        // Enquanto o rosto está ativo, não mostramos texto/ícones/hora.
        return;
    }
    SpiLcdDisplay::SetStatus(status);
}

void RobotFaceDisplay::SetChatMessage(const char* role, const char* content) {
    if (face_mode_) {
        return;
    }
    SpiLcdDisplay::SetChatMessage(role, content);
}

void RobotFaceDisplay::SetEmotion(const char* emotion) {
    if (!face_mode_) {
        SpiLcdDisplay::SetEmotion(emotion);
        return;
    }
    if (!face_widgets_created_ || sleeping_) {
        return;  // a expressão será aplicada quando acordar (ApplyExpression usa expression_)
    }
    RobotExpression e = RobotExpression::kNeutral;
    if (strcmp(emotion, "happy") == 0) e = RobotExpression::kHappy;
    else if (strcmp(emotion, "laughing") == 0) e = RobotExpression::kLaughing;
    else if (strcmp(emotion, "funny") == 0) e = RobotExpression::kFunny;
    else if (strcmp(emotion, "sad") == 0) e = RobotExpression::kSad;
    else if (strcmp(emotion, "angry") == 0) e = RobotExpression::kAngry;
    else if (strcmp(emotion, "crying") == 0) e = RobotExpression::kCrying;
    else if (strcmp(emotion, "loving") == 0) e = RobotExpression::kLoving;
    else if (strcmp(emotion, "embarrassed") == 0) e = RobotExpression::kEmbarrassed;
    else if (strcmp(emotion, "surprised") == 0) e = RobotExpression::kSurprised;
    else if (strcmp(emotion, "shocked") == 0) e = RobotExpression::kShocked;
    else if (strcmp(emotion, "thinking") == 0) e = RobotExpression::kThinking;
    else if (strcmp(emotion, "winking") == 0) e = RobotExpression::kWinking;
    else if (strcmp(emotion, "cool") == 0) e = RobotExpression::kCool;
    else if (strcmp(emotion, "relaxed") == 0) e = RobotExpression::kRelaxed;
    else if (strcmp(emotion, "delicious") == 0) e = RobotExpression::kDelicious;
    else if (strcmp(emotion, "kissy") == 0) e = RobotExpression::kKiss;
    else if (strcmp(emotion, "confident") == 0) e = RobotExpression::kConfident;
    else if (strcmp(emotion, "sleepy") == 0) e = RobotExpression::kSleepy2;
    else if (strcmp(emotion, "silly") == 0) e = RobotExpression::kSilly;
    else if (strcmp(emotion, "confused") == 0) e = RobotExpression::kConfused;
    else e = RobotExpression::kNeutral;  // "neutral" e desconhecidos
    ApplyExpression(e);
}

void RobotFaceDisplay::SetTheme(Theme* theme) {
    // Mantém o comportamento normal (tema da UI de status/texto, usada fora
    // do modo rosto) e, por cima, recolore o rosto também.
    SpiLcdDisplay::SetTheme(theme);

    bool dark = theme != nullptr && theme->name() == "dark";
    if (dark == dark_theme_) return;
    dark_theme_ = dark;

    if (face_widgets_created_) {
        ApplyFaceColors();
    }
    // Se os widgets ainda não foram criados, não precisa fazer nada agora -
    // CreateFaceWidgets() já lê dark_theme_ através de FaceBgColor()/FaceFgColor().
}

// Recolore todos os widgets do rosto já existentes (chamada só quando o tema
// muda em tempo de execução) - não cria nem apaga nenhum objeto, só troca
// propriedades de estilo dos que já existem.
void RobotFaceDisplay::ApplyFaceColors() {
    DisplayLockGuard lock(this);
    lv_color_t fg = FaceFgColor();
    lv_color_t bg = FaceBgColor();

    lv_obj_set_style_bg_color(face_container_, bg, 0);

    lv_obj_set_style_bg_color(left_eye_, fg, 0);
    lv_obj_set_style_bg_color(right_eye_, fg, 0);
    lv_obj_set_style_bg_color(left_pupil_, bg, 0);   // brilho = cor do fundo
    lv_obj_set_style_bg_color(right_pupil_, bg, 0);

    lv_obj_set_style_line_color(left_eye_closed_, fg, 0);
    lv_obj_set_style_line_color(right_eye_closed_, fg, 0);
    lv_obj_set_style_line_color(left_brow_, fg, 0);
    lv_obj_set_style_line_color(right_brow_, fg, 0);
    lv_obj_set_style_line_color(mouth_, fg, 0);

    lv_obj_set_style_bg_color(mouth_open_shape_, fg, 0);
    lv_obj_set_style_bg_color(mouth_teeth_line_, bg, 0);  // linha de "dentes" contrasta com a boca (fg)

    lv_obj_set_style_bg_color(lens_l_, fg, 0);
    lv_obj_set_style_bg_color(lens_r_, fg, 0);
    lv_obj_set_style_line_color(glasses_bridge_, fg, 0);
    lv_obj_set_style_line_color(glasses_arm_l_, fg, 0);
    lv_obj_set_style_line_color(glasses_arm_r_, fg, 0);

    lv_obj_set_style_text_color(zzz_label_, fg, 0);
    // Coração, lágrima, bochecha e língua mantêm cor fixa própria (rosa/azul/
    // vermelho) - não seguem o tema claro/escuro.
}
