import os
from reportlab.lib.pagesizes import letter
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Preformatted, PageBreak
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import inch

def generate_pdf():
    pdf_filename = "EXPLAINED_code_viva_advanced.pdf"
    doc = SimpleDocTemplate(pdf_filename, pagesize=letter, rightMargin=72, leftMargin=72, topMargin=72, bottomMargin=18)
    styles = getSampleStyleSheet()
    
    # Custom styles
    title_style = ParagraphStyle('TitleStyle', parent=styles['Heading1'], fontSize=20, alignment=1, spaceAfter=20)
    heading_style = ParagraphStyle('Heading2', parent=styles['Heading2'], fontSize=16, spaceBefore=15, spaceAfter=10, textColor='#2c3e50')
    subheading_style = ParagraphStyle('Heading3', parent=styles['Heading3'], fontSize=13, spaceBefore=10, spaceAfter=6, textColor='#2980b9')
    
    body_style = ParagraphStyle('BodyStyle', parent=styles['Normal'], fontSize=11, leading=15, spaceBefore=4, spaceAfter=8)
    bullet_style = ParagraphStyle('BulletStyle', parent=styles['Normal'], fontSize=11, leading=15, leftIndent=20, bulletIndent=10, spaceBefore=2, spaceAfter=2)
    
    code_style = ParagraphStyle(
        'CodeStyle',
        parent=styles['Normal'],
        fontName='Courier',
        fontSize=9,
        leading=12,
        spaceBefore=10,
        spaceAfter=10,
        backColor='#f4f4f4',
        borderPadding=5
    )

    story = []

    # Title Page
    story.append(Paragraph("Assignment 2: The Ultimate Comprehensive Viva Guide", title_style))
    story.append(Spacer(1, 0.2 * inch))
    story.append(Paragraph("This document merges the foundational codebase explanations with deep, advanced architectural concepts. It serves as the single source of truth for understanding the pipesim architecture, wiring, hazards, and memory subsystem for your Viva.", body_style))
    story.append(Spacer(1, 0.2 * inch))

    # 1. Pipeline Topology & Wiring
    story.append(Paragraph("1. Core Processor Setup & Wiring (design.cc)", heading_style))
    story.append(Paragraph("The CPU follows a 5-stage RISC pipeline, wired together using a Builder pattern. Data moves through combinational logic stages and is saved in latches at the clock edge.", body_style))
    
    flowchart = """
[PC] --> Fetch --> [IF_ID] --> Decode --> [ID_EX] --> Execute --> [EX_MEM] --> Memory --> [MEM_WB] --> Writeback
    """
    story.append(Preformatted(flowchart, code_style))
    
    story.append(Paragraph("<b>kData vs kClockedData:</b>", subheading_style))
    story.append(Paragraph("`kData` connections (like PC to Fetch) flow continuously during a clock cycle. `kClockedData` connections (like Fetch to IF_ID) capture data at the end of the cycle to be used safely in the next cycle.", body_style))

    story.append(Paragraph("<b>Hazard & Control Wiring:</b>", subheading_style))
    story.append(Paragraph("HazardControl freezes the front-end by sending a `kStall` to PC and IF_ID, and injects a `kBubble` into ID_EX. The Control unit handles branch mispredictions by sending a `kRedirect` to the PC.", body_style))

    # 2. Instruction Lifecycle
    story.append(Paragraph("2. Instruction Lifecycle (design/core.cc)", heading_style))
    story.append(Paragraph("<b>A. Fetch Stage:</b> Interacts strictly with the L1I (Instruction Cache). If there is fetch queue capacity, it issues a request using the PC.", body_style))
    
    story.append(Paragraph("<b>B. Decode Stage (The Brain):</b>", body_style))
    story.append(Paragraph("Performs hazard resolution and dispatch strictly in this order:", bullet_style))
    story.append(Paragraph("1. <i>Completion Capacity:</i> Checks if there is space in the retirement queue.", bullet_style))
    story.append(Paragraph("2. <i>RAW Hazards:</i> Checks `HazardControl` if an older instruction is still calculating a register this instruction needs to read.", bullet_style))
    story.append(Paragraph("3. <i>WAW Hazards:</i> Ensures a younger instruction doesn't overwrite an older one.", bullet_style))
    story.append(Paragraph("4. <i>Unit Availability:</i> Checks if the ALUs are free.", bullet_style))
    story.append(Paragraph("5. <i>Completion Reservation:</i> Reserves a timeslot on the Writeback bus.", bullet_style))
    
    story.append(Paragraph("<b>C. Execute Stage:</b> Dispatches instructions into functional pipelines (e.g., ADD, SUB).", body_style))
    story.append(Paragraph("<b>D. Memory Stage:</b> Non-memory results bypass this. Memory operations use the AGU address to query the L1D cache.", body_style))
    story.append(Paragraph("<b>E. Writeback Stage:</b> Retires exactly 1 instruction per cycle in-order.", body_style))

    story.append(PageBreak())

    # 3. Advanced Superscalar Concepts
    story.append(Paragraph("3. Advanced Superscalar Constraints", heading_style))
    story.append(Paragraph("<b>The Writeback Result Bus Collision:</b>", subheading_style))
    story.append(Paragraph("Because execution units have variable latencies (e.g., ADD=1 cycle, MUL=3 cycles), two instructions issued at different times could finish on the exact same cycle. Since the Writeback bus can only handle 1 result, pipesim implements <b>Completion Slot Reservation</b> at Decode. It calculates `current_boundary_ + latency + 2` and checks if the slot is free. If not, it stalls Decode. This removes the need for complex Reservation Stations.", body_style))

    story.append(Paragraph("<b>Why WAW Hazards are Stalled:</b>", subheading_style))
    story.append(Paragraph("Modern Out-of-Order processors use Register Renaming (e.g., Tomasulo's) to eliminate WAW (Write-After-Write) hazards. Because pipesim does not implement renaming, it must stall the pipeline on a WAW hazard to guarantee safe in-order retirement.", body_style))

    # 4. Cache & Memory Architecture
    story.append(Paragraph("4. Advanced Cache & Memory (cache/)", heading_style))
    story.append(Paragraph("The system uses a Harvard Architecture (Split L1I and L1D) to prevent structural hazards between Fetch and Memory stages.", body_style))
    
    story.append(Paragraph("<b>Non-Blocking Caches & MSHRs:</b>", subheading_style))
    story.append(Paragraph("A blocking cache freezes the CPU on a miss. pipesim uses MSHRs (Miss Status Handling Registers).", body_style))
    story.append(Paragraph("• <i>Hit-Under-Miss:</i> The cache can serve new hits while waiting for a miss to resolve from Main Memory.", bullet_style))
    story.append(Paragraph("• <i>Miss-Under-Miss:</i> Multiple misses to the same cache line are merged into one MSHR, saving memory bandwidth.", bullet_style))

    story.append(Paragraph("<b>Scan Resistance in Replacement Policies:</b>", subheading_style))
    story.append(Paragraph("Standard LRU fails during sequential scans (reading huge arrays), as it evicts useful data for data that won't be used again.", body_style))
    story.append(Paragraph("• <b>Tree-PLRU (Pseudo-LRU):</b> Uses a binary tree of bits to point away from MRU. It's space-efficient but still susceptible to scan thrashing.", bullet_style))
    story.append(Paragraph("• <b>SRRIP (Static Re-Reference Interval Prediction):</b> Solves scan thrashing by inserting new blocks at RRPV=2 (Long Bypass) instead of MRU (0). If they aren't hit quickly, they age to 3 and are evicted. This 'probation period' protects the highly useful data sitting at RRPV=0 from being overwritten.", bullet_style))

    story.append(PageBreak())

    # 5. Master Viva Q&A
    story.append(Paragraph("5. Master Viva Q&A", heading_style))
    
    qa = [
        ("Q1: What is the difference between a Stall and a Bubble?",
         "A: A Stall freezes a latch (like PC or IF_ID) so data isn't lost. A Bubble is a NOP injected downstream (like ID_EX) so the rest of the pipeline can continue emptying out. Without a bubble, downstream stages would execute the same instruction repeatedly."),
        
        ("Q2: In pipesim, the Decode stage stalls on a RAW hazard. How do modern processors handle this?",
         "A: Modern processors use Data Forwarding (Bypassing), passing Execute stage outputs directly back to the Execute inputs of the next instruction. Since pipesim lacks forwarding, it stalls, heavily reducing IPC."),
        
        ("Q3: Why don't we need completion slot reservations for the AGU (Address Generation Unit)?",
         "A: Memory operations go through a cache, where latencies are non-deterministic (due to hits/misses). We can't mathematically predict the exact completion cycle. Instead, memory results are placed in the Completion Queue asynchronously when the cache responds."),
        
        ("Q4: Since Memory Instructions use the AGU, do we need to worry about Memory Disambiguation (Load-After-Store dependencies)?",
         "A: In this architecture, there is only one AGU. Memory instructions are dispatched sequentially. Because they bottleneck through a single AGU in-order, a Store's address is resolved before the subsequent Load is processed, avoiding complex Out-of-Order memory disambiguation issues."),
         
        ("Q5: How does the pipeline recover from a Branch Misprediction?",
         "A: The Control unit evaluates the branch in the Execute stage. If taken, it sends a kRedirect to the PC and flushes all instructions fetched after it using InvalidateFetchesAfter(). The penalty is the depth of the pipeline from Fetch to Execute."),
         
        ("Q6: How does Tree-PLRU differ from True LRU mathematically?",
         "A: True LRU logs exact access order, which requires excessive state bits for highly associative caches. Tree-PLRU uses a binary tree (N-1 bits for N ways) where bits point away from the most recently accessed node. It approximates LRU by following these pointers to find a victim.")
    ]

    for q, a in qa:
        story.append(Paragraph(f"<b>{q}</b>", subheading_style))
        story.append(Paragraph(a, body_style))
        story.append(Spacer(1, 0.15 * inch))

    doc.build(story)
    print("Master Viva Guide generated successfully.")

if __name__ == "__main__":
    generate_pdf()
