import Foundation

@main struct LayoutTest {
    static func main() {
        let config = DashConfig(text: "speed large\nhr medium half\ncadence medium half\nradar medium vertical\npage map\n")
        precondition(config.pages.count == 2)
        let layout = config.pages[0].layout
        precondition(layout.items.count == 4 && layout.verticalItem?.field == "radar")
        precondition(layout.rows.count == 2 && layout.rows[1].count == 2)
        let roundtrip = DashConfig(text: config.configText)
        precondition(roundtrip == config)
        let other = DashLayout(text: "speed small vertical\npower medium vertical\nradar medium\n")
        precondition(other.items.count == 2 && other.verticalItem?.field == "speed")
        precondition(other.rows.count == 1 && other.rows[0][0].field == "power")
        let strip = DashConfig(text: "map radar hr clock\nspeed hero\n")
        precondition(strip.mapFields == ["speed", "hr", "clock"])
        let only = DashLayout(text: "radar medium\n")
        precondition(only.verticalItem?.field == "radar" && only.rows[0][0].field == "speed")
        for height in [50, 75, 100] {
            let text = "speed large\nradar medium half vertical height=\(height)\n"
            let sized = DashLayout(text: text)
            precondition(sized.verticalItem?.heightPercent == height && sized.verticalItem?.half == false)
            precondition(DashLayout(text: sized.configText) == sized)
            let pages = DashConfig(text: text)
            precondition(DashConfig(text: pages.configText) == pages)
            precondition(pages.pages[0].layout.verticalItem?.heightPercent == height)
        }
        precondition(only.verticalItem?.heightPercent == 100)
        precondition(DashLayout(text: "radar medium vertical height=12").verticalItem?.heightPercent == 100)
        let normalized = DashLayout(text: "speed small height=50\nhr medium vertical height=75\npower medium vertical height=50")
        precondition(normalized.items.map(\.heightPercent) == [100, 75, 100])
        let half = DashLayout(text: "radar medium vertical height=50")
        let threeQuarter = DashLayout(text: "radar medium vertical height=75")
        let full = DashLayout(text: "radar medium vertical")
        let grid = [257, 192, 192, 183]
        let navGrid = [214, 161, 161, 150]
        precondition(half.verticalHeight(rowHeights: grid, gutter: 12) == 461)
        precondition(threeQuarter.verticalHeight(rowHeights: grid, gutter: 12) == 665)
        precondition(full.verticalHeight(rowHeights: grid, gutter: 12) == 860)
        precondition(half.verticalHeight(rowHeights: navGrid, gutter: 12) == 387)
        precondition(threeQuarter.verticalHeight(rowHeights: navGrid, gutter: 12) == 560)
        precondition(full.verticalHeight(rowHeights: navGrid, gutter: 12) == 722)
        precondition(half.verticalHeight(rowHeights: [424, 424], gutter: 12) == 424)
        precondition(half.verticalHeight(rowHeights: [860], gutter: 12) == 860)
        precondition(half.verticalHeight(rowHeights: [], gutter: 12) == 0)
        print("Swift radar layout tests passed")
    }
}
