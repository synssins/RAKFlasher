# Contributing to ESP32-RAKFlasher

Thank you for considering contributing to ESP32-RAKFlasher! We welcome contributions from the community.

## How to Contribute

### Reporting Bugs

If you find a bug, please create an issue with:
- Clear description of the problem
- Steps to reproduce
- Expected vs actual behavior
- Hardware configuration (ESP32 model, RAK module version)
- Firmware version
- Serial console output (if applicable)

### Suggesting Features

Feature requests are welcome! Please:
- Check if the feature is already requested
- Clearly describe the feature and use case
- Explain why it would be useful to most users

### Code Contributions

1. **Fork the repository**
2. **Create a feature branch**
   ```bash
   git checkout -b feature/your-feature-name
   ```

3. **Make your changes**
   - Follow the existing code style
   - Add comments for complex logic
   - Update documentation if needed

4. **Test your changes**
   - Ensure code compiles without errors
   - Test on actual hardware if possible
   - Check that WebUI still functions correctly

5. **Commit your changes**
   ```bash
   git commit -m "Add feature: description"
   ```

6. **Push to your fork**
   ```bash
   git push origin feature/your-feature-name
   ```

7. **Create a Pull Request**
   - Provide clear description of changes
   - Reference any related issues
   - Include screenshots for UI changes

## Code Style Guidelines

### C++ Code
- Use 4-space indentation
- Use `#pragma once` for header guards
- Prefer `const` and references where appropriate
- Comment non-obvious logic
- Use meaningful variable names
- Follow existing naming conventions:
  - Classes: `PascalCase`
  - Functions: `camelCase`
  - Variables: `camelCase`
  - Constants: `UPPER_SNAKE_CASE`
  - Member variables: `m_variableName`

### JavaScript
- Use ES6+ features
- Use `const` and `let`, avoid `var`
- Add JSDoc comments for functions
- Keep functions small and focused

### HTML/CSS
- Use semantic HTML5 elements
- Maintain responsive design principles
- Follow existing CSS variable naming
- Keep styles organized by component

## Development Setup

1. **Install PlatformIO**
   ```bash
   pip install platformio
   ```

2. **Clone your fork**
   ```bash
   git clone https://github.com/YOUR_USERNAME/ESP32-RAKFlasher.git
   cd ESP32-RAKFlasher
   ```

3. **Build the project**
   ```bash
   pio run
   ```

4. **Upload to ESP32**
   ```bash
   pio run --target upload
   pio run --target uploadfs
   ```

## Testing

- Test all changes on actual hardware before submitting PR
- Verify WebUI works on different screen sizes
- Check serial console for errors
- Test edge cases and error conditions

## Documentation

- Update README.md if adding features
- Add inline comments for complex code
- Update API documentation if changing endpoints
- Include usage examples where helpful

## Questions?

Feel free to open an issue for:
- Questions about contributing
- Clarification on features
- Help with development setup

## Code of Conduct

Please note that this project follows a [Code of Conduct](CODE_OF_CONDUCT.md). By participating, you agree to abide by its terms.

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
