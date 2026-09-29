#include <Engine/Core/Input.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Core/Glfw.hpp>

namespace Desert::Input
{
    bool Keyboard::IsKeyPressed( Common::KeyCode key )
    {
        auto state = glfwGetKey( static_cast<GLFWwindow*>( EngineContext::GetInstance().GetNativeWindowHandle() ),
                                 static_cast<int>( key ) );
        return state == GLFW_PRESS || state == GLFW_REPEAT;
    }

    Mouse::Mouse() : m_Window( EngineContext::GetInstance().GetNativeWindowHandle() )
    {
    }

    bool Mouse::IsMouseButtonPressed( Common::MouseButton button )
    {
        auto state = glfwGetMouseButton( static_cast<GLFWwindow*>( const_cast<void*>( m_Window ) ), static_cast<int32_t>( button ) );

        return state == GLFW_PRESS;
    }

    float Mouse::GetMouseX()
    {
        auto [x, y] = GetMousePosition();
        return (float)x;
    }

    float Mouse::GetMouseY()
    {
        auto [x, y] = GetMousePosition();
        return (float)y;
    }

    std::pair<float, float> Mouse::GetMousePosition()
    {
        double x, y;
        glfwGetCursorPos( static_cast<GLFWwindow*>( const_cast<void*>( m_Window ) ), &x, &y );
        return { (float)x, (float)y };
    }

    Mouse& Mouse::Get()
    {
        static Mouse s_Instance;
        return s_Instance;
    }

    void Mouse::SetCursorMode( MouseState mode )
    {
        m_MouseMode = mode;

        glfwSetInputMode( static_cast<GLFWwindow*>( const_cast<void*>( m_Window ) ), GLFW_CURSOR, GLFW_CURSOR_NORMAL + (int)mode );
    }

} // namespace Desert::Input
