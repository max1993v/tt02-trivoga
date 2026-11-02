#ifndef _ROBOT_FACE_DISPLAY_H
#define _ROBOT_FACE_DISPLAY_H

#include "display/lcd_display.h"

class AudioCodec;

// As 21 expressões que o app pode mandar via SetEmotion() durante a conversa.
enum class RobotExpression {
    kNeutral, kHappy, kLaughing, kFunny, kSad, kAngry, kCrying, kLoving,
    kEmbarrassed, kSurprised, kShocked, kThinking, kWinking, kCool,
    kRelaxed, kDelicious, kKiss, kConfident, kSleepy2, kSilly, kConfused
};

// Display customizado para a cabeça do robô: mostra a UI normal (status/texto)
// enquanto ligando/conectando, e troca para um "rosto" (olhos com contorno,
// sobrancelhas e boca em linha, fundo preto, sem ícones/hora/texto) assim que
// o dispositivo fica pronto (Idle/Listening/Speaking). As expressões vindas da
// conversa (SetEmotion) mudam o formato dos olhos/sobrancelha/boca.
class RobotFaceDisplay : public SpiLcdDisplay {
public:
    RobotFaceDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                      int width, int height, int offset_x, int offset_y,
                      bool mirror_x, bool mirror_y, bool swap_xy);
    ~RobotFaceDisplay();

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetEmotion(const char* emotion) override;
    // Permite trocar claro/escuro em tempo real (ex.: "coloca no modo
    // escuro" na conversa, via self.screen.set_theme) - recolore os widgets
    // do rosto já existentes, sem recriar nada.
    void SetTheme(Theme* theme) override;

    // Liga o display ao codec de áudio: a boca passa a se mover de acordo com
    // a amplitude real do áudio tocado (em vez de uma animação aleatória).
    void SetAudioCodecForSync(AudioCodec* codec);

private:
    lv_obj_t* face_container_ = nullptr;
    // Olhos estilo "sólido" (preenchido) + brilho (glint) na cor do fundo,
    // em vez do anel+pupila antigo.
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_pupil_ = nullptr;   // brilho (glint), não pupila de verdade
    lv_obj_t* right_pupil_ = nullptr;
    lv_obj_t* left_brow_ = nullptr;
    lv_obj_t* right_brow_ = nullptr;
    lv_obj_t* left_eye_closed_ = nullptr;   // linha RETA usada quando o olho fecha
    lv_obj_t* right_eye_closed_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* mouth_open_shape_ = nullptr;   // forma preenchida usada enquanto fala (boca "com volume")
    lv_obj_t* mouth_teeth_line_ = nullptr;   // linha escura que separa "dentes" da cavidade, tipo desenho animado
    lv_obj_t* mouth_tongue_accent_ = nullptr; // língua visível na boca aberta
    lv_obj_t* zzz_label_ = nullptr;

    // Decorações usadas só em expressões específicas (ficam escondidas no resto).
    // Loving: SEM coração - os olhos ficam com o formato normal (aberto),
    // só que na cor rosa (ver kLovingPink em ApplyExpression). O coração
    // geométrico (2 bolinhas + losango) e a versão com imagem (heart_img.c,
    // que travava com abort() nessa versão do LVGL) foram removidos: além do
    // visual ficar ruim, o widget rotacionado (transform_rotation) travava
    // o C3 ao entrar nessa expressão - removido de todos os aparelhos por
    // consistência visual.
    lv_obj_t* tear_l_ = nullptr, *tear_r_ = nullptr;    // Crying
    lv_obj_t* blush_l_ = nullptr, *blush_r_ = nullptr;  // Embarrassed
    lv_obj_t* tongue_ = nullptr;         // Silly
    // Cool: óculos com 2 lentes + ponte + hastes (em vez de uma barra única).
    lv_obj_t* lens_l_ = nullptr, *lens_r_ = nullptr;
    lv_obj_t* glasses_bridge_ = nullptr, *glasses_arm_l_ = nullptr, *glasses_arm_r_ = nullptr;

    // Buffers de pontos para as linhas (o LVGL só guarda o ponteiro, então
    // precisam ser membros e não variáveis locais).
    lv_point_precise_t brow_l_pts_[3];
    lv_point_precise_t brow_r_pts_[3];
    lv_point_precise_t eye_closed_l_pts_[2];   // linha reta (2 pontos), não mais curva
    lv_point_precise_t eye_closed_r_pts_[2];
    lv_point_precise_t glasses_bridge_pts_[2];
    lv_point_precise_t glasses_arm_l_pts_[2];
    lv_point_precise_t glasses_arm_r_pts_[2];
    static constexpr int kMouthPts = 16;
    lv_point_precise_t mouth_pts_[kMouthPts];

    lv_timer_t* blink_timer_ = nullptr;
    lv_timer_t* blink_end_timer_ = nullptr;
    lv_timer_t* gaze_timer_ = nullptr;
    lv_timer_t* gaze_end_timer_ = nullptr;

    // false = fundo cinza-claro/linhas pretas, true = fundo preto/linhas
    // brancas (padrão original deste aparelho). Trocável em tempo real por
    // SetTheme().
    bool dark_theme_ = true;
    bool face_widgets_created_ = false;
    bool face_mode_ = false;
    bool sleeping_ = false;
    bool eyes_open_ = true;
    bool attentive_ = false;
    bool speaking_ = false;
    int gaze_dx_ = 0, gaze_dy_ = 0;
    RobotExpression expression_ = RobotExpression::kNeutral;
    AudioCodec* audio_codec_ = nullptr;
    float mouth_level_smoothed_ = 0.0f;
    int64_t last_mouth_render_us_ = 0;  // limita a taxa de redesenho da boca

    // Animação da fala por ESTADOS discretos (fechada/pequena/média/aberta),
    // como em desenho animado, em vez de deformar a curva continuamente
    // (isso é o que fazia a boca "vibrar" em vez de parecer abrir/fechar).
    int mouth_state_ = 0;
    int64_t mouth_state_since_us_ = 0;  // tempo mínimo que um estado fica parado antes de trocar

    // Formato "de repouso" da boca da expressão atual; RenderMouth(openness)
    // desenha em cima disso, variando a curva/raio para simular fala.
    bool mouth_is_circle_ = false;
    float mouth_x0_ = 0, mouth_y0_ = 0, mouth_cx_ = 0, mouth_cy_ = 0, mouth_x1_ = 0, mouth_y1_ = 0;
    float mouth_ccx_ = 0, mouth_ccy_ = 0, mouth_cr_ = 0;

    void CreateFaceWidgets();
    lv_color_t FaceBgColor() const;
    lv_color_t FaceFgColor() const;
    void ApplyFaceColors();  // recolore os widgets já criados (troca de tema em tempo real)
    void UpdateFaceModeFromState();
    void EnterFaceMode();
    void ExitFaceMode();
    void SetSleeping(bool sleeping);
    void SetEyesOpen(bool open);
    void SetAttentive(bool attentive);
    void ApplyEyeGeometry();
    void ApplyExpression(RobotExpression expr);
    void RenderMouth(float openness);
    void SetMouthState(int state);  // 0=fechada, 1=pequena, 2=média, 3=aberta
    void OnAudioLevel(float level);  // chamado (task de áudio) a cada bloco tocado
    void ScheduleNextBlink();
    void ScheduleNextGaze();

    static void BlinkTimerCb(lv_timer_t* timer);
    static void BlinkEndTimerCb(lv_timer_t* timer);
    static void GazeTimerCb(lv_timer_t* timer);
    static void GazeEndTimerCb(lv_timer_t* timer);
};

#endif // _ROBOT_FACE_DISPLAY_H
