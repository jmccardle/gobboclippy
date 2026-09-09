#include "Sprite.h"

Sprite::Sprite(std::shared_ptr<Texture> tex, int index)
    : m_tex(std::move(tex)), m_index(index)
{
}

void Sprite::setTexture(std::shared_ptr<Texture> tex)
{
    m_tex = std::move(tex);
}

void Sprite::setSpriteIndex(int index) { m_index = index; }

SDL_FPoint Sprite::contentSize() const
{
    if (!m_tex) return SDL_FPoint{0.0f, 0.0f};
    return SDL_FPoint{(float)m_tex->spriteWidth(), (float)m_tex->spriteHeight()};
}

void Sprite::draw(SDL_Renderer* r, SDL_FPoint offset, float alpha)
{
    if (!m_tex) return;
    const SDL_FRect src = m_tex->frame(m_index);
    blit(r, m_tex->handle(), &src, offset, alpha, color);
}

bool Sprite::setProperty(const std::string& name, int value)
{
    if (name == "sprite_index") { m_index = value; return true; }
    return Drawable::setProperty(name, value);
}

bool Sprite::setProperty(const std::string& name, const SDL_Color& value)
{
    if (name == "color") { color = value; return true; }
    return Drawable::setProperty(name, value);
}

bool Sprite::getProperty(const std::string& name, int& value) const
{
    if (name == "sprite_index") { value = m_index; return true; }
    return Drawable::getProperty(name, value);
}

bool Sprite::getProperty(const std::string& name, SDL_Color& value) const
{
    if (name == "color") { value = color; return true; }
    return Drawable::getProperty(name, value);
}

bool Sprite::hasProperty(const std::string& name) const
{
    return name == "sprite_index" || name == "color" ||
           Drawable::hasProperty(name);
}
