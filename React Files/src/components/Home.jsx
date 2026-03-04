import { useState } from 'react'

export default function Home(props) {
    const [isConnected, setIsConnected] = useState(false)
    const [channel1Checked, setChannel1Checked] = useState(false)
    const [channel2Checked, setChannel2Checked] = useState(false)
    
    const handleButtonClick = () => {
        setIsConnected(!isConnected)
    }
    
    const handleCheckboxChange = (channel) => {
        if (channel === 'channel1') {
            setChannel1Checked(!channel1Checked)
        } else if (channel === 'channel2') {
            setChannel2Checked(!channel2Checked)
        }
    }
    
    return <div className="home card">
        <input 
            type="checkbox" 
            checked={channel1Checked} 
            onChange={() => handleCheckboxChange('channel1')} 
        />
        <label>Channel 1</label>
        <div>
        <input 
            type="checkbox" 
            checked={channel2Checked} 
            onChange={() => handleCheckboxChange('channel2')} 
        />
        <label>Channel 2</label>
        </div>

        <button className="connect" onClick={handleButtonClick}>
            {isConnected ? 'Disconnect' : 'Connect'}
        </button>
        </div>
}