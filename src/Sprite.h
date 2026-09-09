#pragma once
#include "Drawable.h"
#include "Texture.h"

// A drawable frame of a texture. McRogueFace's UISprite, minus the click
// handling and the shader hooks.
//
// `sprite_index` selects the frame from the texture's cell grid. Animating it
// against a list of ints is the frame-sequence animation -- blinking eyes,
// walk cycles, anything an image-generation pipeline emits as a strip.
class Sprite : public Drawable {
public:
    Sprite() = default;
    explicit Sprite(std::shared_ptr<Texture> tex, int index = 0);

    Kind kind() const override { return Kind::Sprite; }

    void setTexture(std::shared_ptr<Texture> tex);
    const std::shared_ptr<Texture>& texture() const { return m_tex; }

    void setSpriteIndex(int index);
    int  spriteIndex() const { return m_index; }

    // Multiplied into the texture on draw; white is "unchanged".
    SDL_Color color {255, 255, 255, 255};

    SDL_FPoint contentSize() const override;

    bool setProperty(const std::string& name, int value) override;
    bool setProperty(const std::string& name, const SDL_Color& value) override;
    bool getProperty(const std::string& name, int& value) const override;
    bool getProperty(const std::string& name, SDL_Color& value) const override;
    bool hasProperty(const std::string& name) const override;

protected:
    void draw(SDL_Renderer* r, SDL_FPoint offset, float alpha) override;

private:
    std::shared_ptr<Texture> m_tex;
    int                      m_index = 0;
};
