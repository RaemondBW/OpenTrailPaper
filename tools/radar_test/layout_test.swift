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
        print("Swift radar layout tests passed")
    }
}
