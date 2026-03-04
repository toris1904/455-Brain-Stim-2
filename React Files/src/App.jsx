import { useState } from 'react'
import './App.css'
import { HashRouter, Routes, Route } from 'react-router'
import Home from './components/Home'

function App() {
  const [count, setCount] = useState(0)

  return <HashRouter>
    <Routes>
        <Route path="/" element={<Home/>}></Route>
    </Routes>
  </HashRouter>
}

export default App
