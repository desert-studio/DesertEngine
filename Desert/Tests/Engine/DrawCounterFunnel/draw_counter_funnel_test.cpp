// ОДНА ВОРОНКА НА ВСЕ ОТРИСОВКИ, и почему это утверждение, а не соглашение.
//
// `Tools/WorldGen` собрал сцену на 50 179 сущностей и смог назвать про неё всё, кроме двух чисел:
// времени кадра и количества draw-вызовов — потому что движок не считал ни одного.
// `Docs/World/PROGRAMME.md` шаг 2 называет этот пробел: вся программа мерится детекторами, а счёт
// отрисовок — то единственное число, которое отличает «рисуем всё подряд» от «рисуем слишком много».
//
// Счётчик поставлен в `Engine/Graphic/DrawCounters`, а все шесть путей отправки в
// `VulkanRenderer.cpp` заведены в две воронки — `DrawIndexedCounted` и `DrawCounted`.
//
// ПОЧЕМУ ЭТО НАДО УТВЕРЖДАТЬ. Счётчик, увеличиваемый в шести местах, — это счётчик, который верен в
// пяти. И ошибку эту не видно ни на одном кадре: **отрисовка, которую не посчитали, выглядит ровно
// как отрисовка, которой не было**. Это та же форма, которую проект находил уже много раз —
// инструмент отвечает не на тот вопрос, что ему задали, и молчит об этом.
//
// Поэтому проверка идёт по ИСХОДНОМУ ТЕКСТУ, тем же приёмом, что `ReservedIdentifiers`: она читает
// дерево, а не собирает его, потому что собранный бинарь о седьмой точке отрисовки не скажет ничего.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    fs::path RepoRoot()
    {
        fs::path p = fs::current_path();
        for ( int i = 0; i < 8; ++i )
        {
            if ( fs::exists( p / "Desert" / "Common" ) && fs::exists( p / "Editor" ) )
            {
                return p;
            }
            p = p.parent_path();
        }
        return {};
    }

    std::vector<std::string> ReadLines( const fs::path& file )
    {
        std::vector<std::string> out;
        std::ifstream            in( file );
        std::string              line;
        while ( std::getline( in, line ) )
        {
            out.push_back( line );
        }
        return out;
    }

    /// Строка, которая ВЫЗЫВАЕТ vk-отрисовку, а не просто называет её в прозе.
    bool IsDrawCall( const std::string& line )
    {
        // Комментарий — не код: этот файл и заголовок рендерера обязаны называть запрещаемое, и
        // перепись, краснеющая на собственном объяснении, будет заглушена первым же, кому помешает.
        const std::size_t firstGlyph = line.find_first_not_of( " \t" );
        if ( firstGlyph != std::string::npos &&
             ( line.compare( firstGlyph, 2, "//" ) == 0 || line.compare( firstGlyph, 1, "*" ) == 0 ) )
        {
            return false;
        }
        return line.find( "vkCmdDraw( " ) != std::string::npos ||
               line.find( "vkCmdDrawIndexed( " ) != std::string::npos;
    }
} // namespace

// П1. НИ ОДНОЙ ОТРИСОВКИ МИМО ВОРОНОК. Число не вписано: оно выведено из того, сколько вызовов у
// функций, объявленных ровно для этого. Так перепись нельзя удовлетворить, поправив число.
TEST( DrawCounterFunnel, NoDrawEscapesTheCountedFunnels )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "дерево не найдено — эта перепись не осмотрела ничего";

    const fs::path renderer = root / "Desert" / "Desert" / "Source" / "Engine" / "Graphic" / "API" /
                              "Vulkan" / "VulkanRenderer.cpp";
    ASSERT_TRUE( fs::exists( renderer ) ) << renderer.generic_string();

    const std::vector<std::string> lines = ReadLines( renderer );
    ASSERT_FALSE( lines.empty() ) << "файл рендерера прочитан пустым";

    // Строка считается разрешённой, только если она стоит В ТЕЛЕ одной из воронок. Тело определяется
    // грубо и намеренно: от строки с определением до следующей строки, начинающейся с четырёх пробелов
    // и закрывающей скобки. Точный разбор C++ здесь был бы дороже предмета.
    std::vector<std::string> offenders;
    bool                     insideFunnel = false;
    for ( std::size_t i = 0; i < lines.size(); ++i )
    {
        const std::string& line = lines[i];
        if ( line.find( "VulkanRendererAPI::DrawIndexedCounted(" ) != std::string::npos ||
             line.find( "VulkanRendererAPI::DrawCounted(" ) != std::string::npos )
        {
            insideFunnel = true;
        }
        else if ( insideFunnel && line == "    }" )
        {
            insideFunnel = false;
        }

        if ( IsDrawCall( line ) && !insideFunnel )
        {
            offenders.push_back( "VulkanRenderer.cpp:" + std::to_string( i + 1 ) + ": " + line );
        }
    }

    EXPECT_TRUE( offenders.empty() )
         << "Отрисовка записана мимо счётчика. Она не попадёт ни в одно число, и в кадре это выглядит\n"
            "ровно как отрисовка, которой не было. Заведи её через DrawIndexedCounted/DrawCounted.\n"
         << [&offenders]
    {
        std::string all;
        for ( const std::string& o : offenders )
        {
            all += "  " + o + "\n";
        }
        return all;
    }();
}

// П2. И ВОРОНКИ СУЩЕСТВУЮТ. Без этого П1 проходит на файле, из которого отрисовку убрали целиком —
// то есть на состоянии, где счётчик верен и бесполезен. Положительный контроль к П1.
TEST( DrawCounterFunnel, TheFunnelsThemselvesStillRecord )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::vector<std::string> lines = ReadLines( root / "Desert" / "Desert" / "Source" / "Engine" /
                                                      "Graphic" / "API" / "Vulkan" / "VulkanRenderer.cpp" );
    ASSERT_FALSE( lines.empty() );

    int draws = 0;
    int records = 0;
    for ( const std::string& line : lines )
    {
        if ( IsDrawCall( line ) )
        {
            ++draws;
        }
        if ( line.find( "DrawCounter::Record(" ) != std::string::npos )
        {
            ++records;
        }
    }

    EXPECT_EQ( draws, 2 ) << "ожидались ровно две vk-отрисовки — по одной в каждой воронке";
    EXPECT_EQ( records, draws ) << "каждая воронка обязана считать РОВНО один раз: " << records
                                << " вызовов Record на " << draws << " отрисовок";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
